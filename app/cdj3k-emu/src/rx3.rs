//! RX3 hardware window around the actual firmware LCD.
mod hardware;
use cdj3k_emu_panel::rx3::Rx3Panel;
use std::{
    fs::{File, OpenOptions},
    io::{self, Write},
    os::fd::AsRawFd,
    os::unix::{
        fs::{FileExt, MetadataExt},
        net::UnixStream,
    },
    path::PathBuf,
    time::{Duration, Instant},
};

pub fn run(args: &[String]) {
    let run = args
        .iter()
        .position(|s| s == "--rx3")
        .and_then(|i| args.get(i + 1))
        .filter(|s| !s.starts_with('-'))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("build/rx3/app"));
    let lock = match OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .truncate(false)
        .open(run.join("window.lock"))
    {
        Ok(lock) => lock,
        Err(error) => {
            eprintln!("RX3 run directory: {error}");
            return;
        }
    };
    // A QEMU chardev accepts one client. A second connect can sit in its
    // backlog and look successful while delivering no controls.
    if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        eprintln!("An RX3 window is already connected to {}", run.display());
        return;
    }
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1400., 940.])
            .with_min_inner_size([850., 590.]),
        ..Default::default()
    };
    eframe::run_native(
        "XDJ-RX3",
        options,
        Box::new(move |cc| {
            cc.egui_ctx.set_visuals(egui::Visuals::dark());
            super::configure_helvetica_medium(&cc.egui_ctx);
            Ok(Box::new(Rx3App::new(run, lock)))
        }),
    )
    .expect("RX3 window");
}
struct Rx3App {
    run: PathBuf,
    _lock: File,
    panel: Rx3Panel,
    socket: Option<UnixStream>,
    lcd: Option<File>,
    texture: Option<egui::TextureHandle>,
    last_pixels: Vec<u8>,
    last_refresh: Instant,
    touch_seq: u32,
    touching: bool,
    release: Vec<(u16, usize, Instant)>,
    status: String,
}
impl Rx3App {
    fn new(run: PathBuf, lock: File) -> Self {
        Self {
            run,
            _lock: lock,
            panel: Rx3Panel::default(),
            socket: None,
            lcd: None,
            texture: None,
            last_pixels: Vec::new(),
            last_refresh: Instant::now() - Duration::from_secs(1),
            touch_seq: 0,
            touching: false,
            release: vec![],
            status: "Waiting for guest".into(),
        }
    }
    fn send(&mut self) {
        if self.socket.is_none() {
            self.socket = UnixStream::connect(self.run.join("panel.sock")).ok();
            if let Some(s) = &self.socket {
                let _ = s.set_write_timeout(Some(Duration::from_millis(20)));
            }
        }
        if let Some(s) = &mut self.socket {
            if let Err(e) = s.write_all(&self.panel.records()) {
                self.status = e.to_string();
                if let Ok(mut log) = OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(self.run.join("input.log"))
                {
                    let _ = writeln!(log, "transport: {e}");
                }
                self.socket = None;
            } else {
                self.status = "Panel connected".into();
            }
        }
    }
    fn pulse(&mut self, key: u16, deck: usize) {
        if let Ok(mut log) = OpenOptions::new()
            .create(true)
            .append(true)
            .open(self.run.join("input.log"))
        {
            let _ = writeln!(log, "press {key:04x} deck={deck}");
        }
        if !self.panel.key(key, deck, true) {
            return;
        }
        self.send();
        self.release.retain(|(k, d, _)| (*k, *d) != (key, deck));
        self.release
            .push((key, deck, Instant::now() + Duration::from_millis(120)));
    }
    fn refresh(&mut self, ctx: &egui::Context) -> io::Result<()> {
        if let Some(f) = &self.lcd {
            if f.metadata()?.ino() != std::fs::metadata(self.run.join("rx3fb.shm"))?.ino() {
                self.lcd = None;
                self.last_pixels.clear();
            }
        }
        if self.lcd.is_none() {
            self.lcd = Some(
                OpenOptions::new()
                    .read(true)
                    .write(true)
                    .open(self.run.join("rx3fb.shm"))?,
            );
        }
        if self.last_refresh.elapsed() < Duration::from_millis(33) {
            return Ok(());
        }
        self.last_refresh = Instant::now();
        let f = self.lcd.as_ref().unwrap();
        let mut h = [0u8; 36];
        f.read_exact_at(&mut h, 0)?;
        let get = |i: usize| u32::from_le_bytes(h[i * 4..i * 4 + 4].try_into().unwrap());
        if get(0) != 0x46335852 || get(5) != 16 {
            return Ok(());
        }
        let (gen, w, height, stride, yoff, offset) = (
            get(2),
            get(3) as usize,
            get(4) as usize,
            get(6) as usize,
            get(7) as usize,
            get(8) as usize,
        );
        if gen & 1 != 0 || w == 0 || height == 0 || w > 2048 || height > 2048 || stride < w * 2 {
            return Ok(());
        }
        let size = stride
            .checked_mul(height)
            .ok_or_else(|| io::Error::other("LCD size"))?;
        if offset as u64 + ((yoff + height) * stride) as u64 > f.metadata()?.len() {
            return Ok(());
        }
        let mut pixels = vec![0; size];
        f.read_exact_at(&mut pixels, (offset + yoff * stride) as u64)?;
        // The generation protects mode/pan metadata, not writes through the
        // firmware's mmap. DirectFB also draws in-place without panning.
        if pixels == self.last_pixels {
            return Ok(());
        }
        let mut rgb = Vec::with_capacity(w * height * 3);
        for row in pixels.chunks_exact(stride) {
            for p in row[..w * 2].as_chunks::<2>().0 {
                let v = u16::from_le_bytes([p[0], p[1]]);
                rgb.extend_from_slice(&[
                    ((v >> 11) * 255 / 31) as u8,
                    (((v >> 5) & 63) * 255 / 63) as u8,
                    ((v & 31) * 255 / 31) as u8,
                ]);
            }
        }
        let image = egui::ColorImage::from_rgb([w, height], &rgb);
        if let Some(t) = &mut self.texture {
            t.set(image, egui::TextureOptions::LINEAR);
        } else {
            self.texture = Some(ctx.load_texture("RX3 LCD", image, egui::TextureOptions::LINEAR));
        }
        self.last_pixels = pixels;
        Ok(())
    }
    fn touch(&mut self, down: bool, x: u32, y: u32) {
        if let Some(f) = &self.lcd {
            self.touch_seq = self.touch_seq.wrapping_add(2);
            let odd = self.touch_seq | 1;
            let _ = f.write_all_at(&odd.to_le_bytes(), 0x800);
            let mut data = Vec::new();
            for v in [u32::from(down), x, y] {
                data.extend_from_slice(&v.to_le_bytes());
            }
            let _ = f.write_all_at(&data, 0x804);
            let _ = f.write_all_at(&self.touch_seq.to_le_bytes(), 0x800);
        }
        self.touching = down;
    }
}
impl eframe::App for Rx3App {
    fn logic(&mut self, ctx: &egui::Context, _: &mut eframe::Frame) {
        // egui can run multiple UI passes inside one rendered frame. Keep
        // a click asserted across the panel's sampling/debounce interval.
        let now = Instant::now();
        let pending = std::mem::take(&mut self.release);
        let mut changed = false;
        for (key, deck, deadline) in pending {
            if now >= deadline {
                self.panel.key(key, deck, false);
                changed = true;
            } else {
                self.release.push((key, deck, deadline));
            }
        }
        if changed {
            self.send();
        }
        if self.socket.is_none() {
            self.send();
        }
        ctx.request_repaint_after(Duration::from_millis(16));
    }
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        let _ = self.refresh(ui.ctx());
        self.hardware(ui);
    }
}
