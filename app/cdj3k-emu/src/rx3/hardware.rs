//! RX3 top panel, drawn in the proportions of the manufacturer's top view.
//! All positions use one 1982 × 1280 reference canvas, including LCD hit tests.
use super::Rx3App;
use egui::{Align2, Color32 as C, FontId, Painter, Pos2, Rect, Sense, Stroke, StrokeKind, Vec2};
use std::f32::consts::TAU;

const WHITE: C = C::from_gray(207);
const SILVER: C = C::from_gray(114);
const BLUE: C = C::from_rgb(48, 110, 245);
const ORANGE: C = C::from_rgb(244, 153, 39);
const GREEN: C = C::from_rgb(76, 199, 88);
const RED: C = C::from_rgb(226, 56, 61);
struct Canvas {
    origin: Pos2,
    scale: f32,
    painter: Painter,
}
impl Canvas {
    fn p(&self, x: f32, y: f32) -> Pos2 {
        self.origin + Vec2::new(x, y) * self.scale
    }
    fn rect(&self, x: f32, y: f32, w: f32, h: f32) -> Rect {
        Rect::from_min_size(self.p(x, y), Vec2::new(w, h) * self.scale)
    }
    fn line(&self, a: [f32; 2], b: [f32; 2], color: C, width: f32) {
        self.painter.line_segment(
            [self.p(a[0], a[1]), self.p(b[0], b[1])],
            Stroke::new(width * self.scale, color),
        );
    }
    #[allow(clippy::too_many_arguments)] // Canvas geometry and paint style.
    fn box_(&self, x: f32, y: f32, w: f32, h: f32, r: f32, fill: C, stroke: C) {
        self.painter.rect(
            self.rect(x, y, w, h),
            r * self.scale,
            fill,
            Stroke::new(self.scale, stroke),
            StrokeKind::Inside,
        );
    }
    fn text(&self, x: f32, y: f32, text: &str, size: f32, color: C) {
        self.painter.text(
            self.p(x, y),
            Align2::CENTER_CENTER,
            text,
            FontId::proportional(size * self.scale),
            color,
        );
    }
    fn circle(&self, x: f32, y: f32, r: f32, fill: C, stroke: C, width: f32) {
        self.painter.circle(
            self.p(x, y),
            r * self.scale,
            fill,
            Stroke::new(width * self.scale, stroke),
        );
    }
    fn knob(&self, x: f32, y: f32, r: f32, value: f32) {
        self.circle(x + 2., y + 3., r + 5., C::BLACK, C::from_gray(42), 1.);
        self.circle(x, y, r, C::from_gray(46), C::from_gray(155), 2.);
        self.circle(x, y, r - 4., C::from_gray(21), C::from_gray(56), 1.);
        for i in 0..20 {
            let a = i as f32 * TAU / 20.;
            self.line(
                [x + a.cos() * (r - 3.), y + a.sin() * (r - 3.)],
                [x + a.cos() * r, y + a.sin() * r],
                C::from_gray(82),
                1.,
            );
        }
        let a = (-225. + value * 270.).to_radians();
        self.line(
            [x + a.cos() * r * 0.28, y + a.sin() * r * 0.28],
            [x + a.cos() * (r - 2.), y + a.sin() * (r - 2.)],
            WHITE,
            3.,
        );
    }
}
impl Rx3App {
    pub(super) fn hardware(&mut self, ui: &mut egui::Ui) {
        let scale = (ui.available_width() / 1982.).min(ui.available_height() / 1280.);
        let left_padding = (ui.available_width() - 1982. * scale).max(0.) * 0.5;
        let (area, _) = ui.allocate_exact_size(Vec2::new(1982., 1280.) * scale, Sense::hover());
        let c = Canvas {
            origin: area.min + Vec2::new(left_padding, 0.),
            scale,
            painter: ui.painter_at(area.translate(Vec2::new(left_padding, 0.))),
        };
        c.box_(0., 252., 1982., 1028., 15., C::from_gray(12), SILVER);
        c.box_(
            7.,
            259.,
            1968.,
            1014.,
            12.,
            C::from_gray(19),
            C::from_gray(56),
        );
        // Brushed lower fascia, with divisions between decks and mixer.
        for x in (12..1975).step_by(4) {
            c.line(
                [x as f32, 439.],
                [x as f32, 1267.],
                C::from_gray(if x % 12 == 0 { 25 } else { 21 }),
                0.6,
            );
        }
        for x in [648., 690., 1142., 1294., 1333.] {
            c.line([x, 485.], [x, 1268.], C::from_gray(57), 1.);
        }
        c.line([8., 435.], [650., 435.], SILVER, 1.);
        c.line([1438., 435.], [1974., 435.], SILVER, 1.);
        // The raised display and the navigation pod have their own outlines.
        c.box_(1317., 108., 121., 310., 7., C::from_gray(18), SILVER);
        c.box_(650., 0., 684., 489., 4., C::from_gray(18), SILVER);
        c.box_(658., 28., 668., 450., 2., C::from_gray(9), C::from_gray(66));
        c.text(990., 15., "Pioneer DJ", 27., WHITE);
        for (i, (name, key)) in [
            ("SOURCE", 0x0201),
            ("BROWSE", 0x0202),
            ("TAG LIST", 0x0203),
            ("PLAYLIST", 0x0204),
            ("SEARCH", 0x0205),
            ("MENU", 0x0206),
        ]
        .into_iter()
        .enumerate()
        {
            self.hw_button(
                ui,
                &c,
                705. + i as f32 * 99.,
                53.,
                80.,
                22.,
                name,
                key,
                0,
                if i == 0 || i == 5 { WHITE } else { BLUE },
            );
        }
        c.box_(692., 91., 603., 379., 1., C::BLACK, C::from_gray(62));
        let lcd = c.rect(701., 100., 585., 365.625);
        if let Some(t) = &self.texture {
            c.painter.image(
                t.id(),
                lcd,
                Rect::from_min_max(Pos2::ZERO, Pos2::new(1., 1.)),
                C::WHITE,
            );
        }
        let response = ui.interact(lcd, ui.id().with("rx3-lcd"), Sense::drag());
        let down = ui.input(|i| i.pointer.primary_down());
        if down && response.contains_pointer() {
            if let Some(p) = ui.input(|i| i.pointer.interact_pos()) {
                let v = (p - lcd.min) / lcd.size();
                self.touch(
                    true,
                    (v.x * 1279.).clamp(0., 1279.) as u32,
                    (v.y * 799.).clamp(0., 799.) as u32,
                );
            }
        } else if self.touching {
            self.touch(false, 0, 0);
        }
        self.hw_button(ui, &c, 1328., 145., 39., 48., "BACK", 0x420d, 0, WHITE);
        self.hw_button(ui, &c, 1370., 145., 47., 48., "TAG TRACK", 0x420e, 0, WHITE);
        self.hw_button(
            ui,
            &c,
            1329.,
            309.,
            39.,
            38.,
            "TRACK\nFILTER",
            0x420f,
            0,
            WHITE,
        );
        self.hw_button(
            ui,
            &c,
            1371.,
            309.,
            45.,
            38.,
            "SHORT\nCUT",
            0x0210,
            0,
            WHITE,
        );
        c.knob(1370., 249., 54., 0.5);
        c.circle(1370., 249., 26., C::from_gray(34), WHITE, 2.);
        let browse = ui.interact(
            c.rect(1315., 195., 110., 108.),
            ui.id().with("browse-knob"),
            Sense::click_and_drag(),
        );
        if browse.clicked() {
            self.pulse(0x420c, 0);
        }
        let wheel = if browse.hovered() {
            ui.input(|i| i.smooth_scroll_delta.y)
        } else {
            0.
        };
        if wheel.abs() > 0.5 || browse.drag_delta().y.abs() > 3. {
            let delta = if wheel != 0. {
                -wheel.signum() as i16
            } else {
                browse.drag_delta().y.signum() as i16
            };
            self.panel.browse(delta);
            self.send();
        }
        c.text(1371., 361., "LOAD", 12., WHITE);
        self.hw_button(ui, &c, 1336., 370., 30., 27., "1", 0x4311, 1, WHITE);
        self.hw_button(ui, &c, 1376., 370., 30., 27., "2", 0x4311, 2, WHITE);
        c.text(1371., 405., "INST. DOUBLES", 8., WHITE);
        c.text(330., 225., &self.status, 12., SILVER);
        // Microphone and rear USB bank.
        c.text(323., 292., "MIC", 14., WHITE);
        c.line([100., 292.], [298., 292.], WHITE, 1.);
        c.line([347., 292.], [550., 292.], WHITE, 1.);
        for (x, label) in [
            (270., "LOW"),
            (352., "HI"),
            (435., "MIC 1"),
            (517., "MIC 2"),
        ] {
            c.text(x, 320., label, 11., WHITE);
            c.knob(x, 353., 21., 0.5);
        }
        c.box_(111., 335., 64., 39., 5., C::BLACK, SILVER);
        c.box_(134., 345., 22., 18., 2., C::from_gray(65), SILVER);
        c.text(143., 318., "OFF · ON · TALK OVER", 9., WHITE);
        c.text(1540., 315., "rekordbox", 25., WHITE);
        c.text(1540., 350., "serato", 22., WHITE);
        c.text(1540., 381., "MP3 / AAC / WAV / AIFF / FLAC", 10., WHITE);
        for (x, n) in [(1686., 1), (1806., 2)] {
            c.text(x, 278., &format!("USB {n}"), 13., WHITE);
            c.text(x, 292., "5V ⎓ 1A", 9., WHITE);
            c.box_(x - 28., 301., 56., 48., 3., C::BLACK, SILVER);
            c.box_(
                x - 20.,
                307.,
                40.,
                30.,
                2.,
                C::from_gray(32),
                C::from_gray(49),
            );
            c.text(x, 369., "USB STOP", 10., WHITE);
            self.hw_round(ui, &c, x, 400., 13., "", 0x8002, n, RED);
        }
        c.text(1907., 301., "MASTER REC", 11., WHITE);
        c.circle(1907., 323., 11., C::from_rgb(61, 19, 21), SILVER, 2.);
        c.text(1907., 369., "TRACK MARK", 10., WHITE);
        c.circle(1907., 400., 12., C::from_gray(26), SILVER, 1.);
        // Both physical decks use the same local geometry.
        for deck in 1..=2 {
            self.hw_deck(ui, &c, deck);
        }
        self.hw_mixer(ui, &c);
    }
    #[allow(clippy::too_many_arguments)] // Physical placement plus firmware control identity.
    fn hw_button(
        &mut self,
        ui: &mut egui::Ui,
        c: &Canvas,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        label: &str,
        key: u16,
        deck: usize,
        color: C,
    ) {
        let r = ui.interact(
            c.rect(x, y, w, h),
            ui.id().with(("rx3-key", key, deck)),
            Sense::click(),
        );
        c.box_(
            x,
            y,
            w,
            h,
            3.,
            C::from_gray(if r.is_pointer_button_down_on() {
                57
            } else {
                15
            }),
            SILVER,
        );
        c.box_(
            x + 3.,
            y + 3.,
            w - 6.,
            h - 6.,
            2.,
            C::from_gray(22),
            C::from_gray(42),
        );
        c.text(
            x + w / 2.,
            y + h / 2.,
            label,
            if h > 30. { 10. } else { 9. },
            color,
        );
        if r.clicked() {
            self.pulse(key, deck);
        }
    }
    #[allow(clippy::too_many_arguments)] // Physical placement plus firmware control identity.
    fn hw_round(
        &mut self,
        ui: &mut egui::Ui,
        c: &Canvas,
        x: f32,
        y: f32,
        r: f32,
        label: &str,
        key: u16,
        deck: usize,
        color: C,
    ) {
        let response = ui.interact(
            c.rect(x - r, y - r, r * 2., r * 2.),
            ui.id().with(("rx3-key", key, deck)),
            Sense::click(),
        );
        c.circle(x, y, r + 3., C::BLACK, C::from_gray(71), 2.);
        c.circle(
            x,
            y,
            r,
            C::from_gray(if response.is_pointer_button_down_on() {
                67
            } else {
                31
            }),
            color,
            3.,
        );
        c.circle(x, y, r - 5., C::TRANSPARENT, C::from_gray(75), 1.);
        c.text(x, y, label, if r > 25. { 13. } else { 9. }, WHITE);
        if response.clicked() {
            self.pulse(key, deck);
        }
    }
    fn hw_deck(&mut self, ui: &mut egui::Ui, c: &Canvas, deck: usize) {
        let dx = if deck == 1 { 0. } else { 1280. };
        c.text(dx + 137., 450., "IN / 4BEAT", 11., WHITE);
        c.text(dx + 202., 450., "OUT", 11., WHITE);
        c.text(dx + 284., 450., "RELOOP/EXIT", 10., WHITE);
        self.hw_round(ui, c, dx + 132., 485., 20., "", 0x410c, deck, ORANGE);
        self.hw_round(ui, c, dx + 202., 485., 20., "", 0x410d, deck, ORANGE);
        self.hw_round(ui, c, dx + 284., 485., 13., "●", 0x410e, deck, ORANGE);
        c.text(dx + 132., 520., "IN ADJUST", 9., WHITE);
        c.text(dx + 202., 520., "OUT ADJUST", 9., WHITE);
        c.text(dx + 431., 457., "CUE / LOOP", 11., WHITE);
        self.hw_round(ui, c, dx + 404., 485., 12., "◀", 0x4124, deck, WHITE);
        self.hw_round(ui, c, dx + 462., 485., 12., "▶", 0x4125, deck, WHITE);
        c.text(dx + 431., 506., "CALL", 9., WHITE);
        c.text(dx + 526., 456., "DELETE", 10., WHITE);
        c.text(dx + 582., 456., "MEMORY", 10., WHITE);
        self.hw_round(ui, c, dx + 526., 485., 14., "", 0x4322, deck, WHITE);
        self.hw_round(ui, c, dx + 582., 485., 12., "", 0x4323, deck, WHITE);
        for (x, label, key) in [
            (74., "SLIP", 0x4110),
            (146., "QUANTIZE", 0x410b),
            (631., "VINYL SPEED ADJ.", 0x4121),
        ] {
            c.text(dx + x, 549., label, 10., WHITE);
            self.hw_round(ui, c, dx + x, 574., 14., "●", key, deck, RED);
        }
        c.text(dx + 530., 536., "JOG ADJUST", 11., WHITE);
        c.knob(dx + 530., 574., 22., 0.5);
        c.text(dx + 530., 605., "LIGHT · HEAVY", 9., WHITE);
        c.text(dx + 74., 617., "DIRECTION", 11., WHITE);
        self.hw_button(ui, c, dx + 49., 629., 50., 32., "REV", 0x410f, deck, RED);
        c.text(dx + 631., 616., "JOG MODE", 11., WHITE);
        self.hw_button(
            ui,
            c,
            dx + 607.,
            632.,
            48.,
            26.,
            "VINYL",
            0x4104,
            deck,
            BLUE,
        );
        c.text(dx + 74., 712., "TRACK SEARCH", 11., WHITE);
        self.hw_round(ui, c, dx + 52., 741., 17., "◀|", 0x4215, deck, WHITE);
        self.hw_round(ui, c, dx + 96., 741., 17., "|▶", 0x4214, deck, WHITE);
        c.text(dx + 74., 792., "SEARCH", 11., WHITE);
        self.hw_round(ui, c, dx + 52., 824., 17., "◀◀", 0x4120, deck, WHITE);
        self.hw_round(ui, c, dx + 96., 824., 17., "▶▶", 0x411f, deck, WHITE);
        self.hw_button(
            ui,
            c,
            dx + 52.,
            911.,
            45.,
            25.,
            "SHIFT",
            0x4103,
            deck,
            WHITE,
        );
        c.text(dx + 631., 688., "BEAT SYNC", 11., WHITE);
        self.hw_button(
            ui,
            c,
            dx + 598.,
            704.,
            30.,
            31.,
            "SYNC",
            0x4112,
            deck,
            WHITE,
        );
        self.hw_button(
            ui,
            c,
            dx + 638.,
            704.,
            30.,
            31.,
            "MASTER",
            0x4111,
            deck,
            ORANGE,
        );
        c.text(dx + 631., 778., "TEMPO", 12., WHITE);
        c.text(dx + 631., 792., "±6 / ±10 / ±16 / WIDE", 8., WHITE);
        self.hw_round(ui, c, dx + 631., 810., 11., "", 0x4107, deck, WHITE);
        c.text(dx + 631., 845., "MASTER", 11., WHITE);
        c.text(dx + 631., 858., "TEMPO", 11., WHITE);
        self.hw_round(ui, c, dx + 631., 880., 13., "●", 0x4108, deck, RED);
        c.box_(dx + 600., 911., 63., 333., 6., C::from_gray(11), SILVER);
        c.line([dx + 631., 940.], [dx + 631., 1207.], C::BLACK, 6.);
        for i in 0..31 {
            let y = 940. + i as f32 * 8.6;
            c.line([dx + 610., y], [dx + 625., y], SILVER, 1.);
            c.line([dx + 638., y], [dx + 653., y], SILVER, 1.);
        }
        c.box_(dx + 607., 1050., 49., 16., 2., C::from_gray(48), SILVER);
        c.line([dx + 610., 1058.], [dx + 653., 1058.], WHITE, 3.);
        c.text(dx + 631., 1228., "TEMPO", 10., WHITE);
        // Concentric machined jog platter with rubber scallops. The center
        // stays dark until the separate ready-channel LCD protocol is recovered.
        let (x, y) = (dx + 359., 783.);
        c.circle(x, y, 224., C::BLACK, C::from_gray(58), 3.);
        c.circle(x, y, 215., C::from_gray(33), C::from_gray(73), 2.);
        for i in 0..28 {
            let a = i as f32 * TAU / 28.;
            c.circle(
                x + a.cos() * 204.,
                y + a.sin() * 204.,
                12.,
                C::from_gray(16),
                C::from_gray(43),
                1.,
            );
        }
        c.circle(x, y, 190., C::from_gray(25), C::from_gray(60), 2.);
        for r in (90..185).step_by(3) {
            c.circle(
                x,
                y,
                r as f32,
                C::TRANSPARENT,
                C::from_gray(if r % 2 == 0 { 35 } else { 29 }),
                0.6,
            );
        }
        c.circle(x, y, 91., C::from_gray(11), SILVER, 1.);
        c.circle(x, y, 69., C::BLACK, C::from_gray(39), 2.);
        c.text(x, y + 83., "BEAT SYNC", 8., WHITE);
        c.text(dx + 92., 999., "CUE", 10., WHITE);
        self.hw_round(ui, c, dx + 92., 1059., 44., "CUE", 0x4102, deck, ORANGE);
        c.text(dx + 92., 1130., "PLAY / PAUSE", 11., WHITE);
        self.hw_round(ui, c, dx + 92., 1190., 44., "▶ / Ⅱ", 0x4101, deck, GREEN);
        c.line([dx + 185., 1027.], [dx + 519., 1027.], WHITE, 1.);
        c.text(dx + 353., 1027., "PAD MODE", 10., WHITE);
        for (i, (label, key, color)) in [
            ("HOT CUE", 0x4113, WHITE),
            ("BEAT LOOP", 0x4114, ORANGE),
            ("SLIP LOOP", 0x4115, BLUE),
            ("BEAT JUMP", 0x4116, RED),
        ]
        .into_iter()
        .enumerate()
        {
            self.hw_button(
                ui,
                c,
                dx + 199. + i as f32 * 82.,
                1046.,
                65.,
                20.,
                label,
                key,
                deck,
                color,
            );
        }
        let colors = [
            RED,
            C::from_rgb(49, 206, 232),
            C::from_rgb(166, 202, 37),
            C::from_rgb(90, 78, 222),
            GREEN,
            ORANGE,
            BLUE,
            C::from_rgb(218, 198, 49),
        ];
        for (n, &color) in colors.iter().enumerate() {
            let px = dx + 199. + (n % 4) as f32 * 82.;
            let py = 1097. + (n / 4) as f32 * 82.;
            self.hw_button(ui, c, px, py, 64., 64., "", 0x4117 + n as u16, deck, color);
            c.painter.rect_stroke(
                c.rect(px + 3., py + 3., 58., 58.),
                2. * c.scale,
                Stroke::new(3. * c.scale, color),
                StrokeKind::Inside,
            );
        }
        c.text(dx + 350., 1257., "HOT CUE / DELETE", 9., WHITE);
    }
    #[allow(clippy::too_many_arguments)] // Physical placement plus firmware control identity.
    fn hw_knob(
        &mut self,
        ui: &mut egui::Ui,
        c: &Canvas,
        x: f32,
        y: f32,
        r: f32,
        label: &str,
        key: u16,
        deck: usize,
        value: u16,
    ) {
        c.text(x, y - r - 16., label, 11., WHITE);
        c.knob(x, y, r, value as f32 / 1023.);
        let response = ui.interact(
            c.rect(x - r - 6., y - r - 6., r * 2. + 12., r * 2. + 12.),
            ui.id().with(("rx3-analog", key, deck)),
            Sense::drag(),
        );
        if response.dragged() {
            let delta = ui.input(|i| i.pointer.delta().y);
            let value = (value as f32 - delta * 4. / c.scale).clamp(0., 1023.) as u16;
            if self.panel.analog(key, deck, value) {
                self.send();
            }
        }
        if response.double_clicked() {
            self.panel.analog(key, deck, 512);
            self.send();
        }
    }
    fn hw_mixer(&mut self, ui: &mut egui::Ui, c: &Canvas) {
        for deck in 1..=2 {
            let x = if deck == 1 { 895. } else { 1087. };
            let base = 36 + (deck - 1) * 16;
            for (y, label, key, off) in [
                (540., "TRIM", 0x5019, 2),
                (612., "HI", 0x501a, 0),
                (688., "MID", 0x501b, 4),
                (766., "LOW", 0x501c, 6),
                (848., "COLOR", 0x501d, 8),
            ] {
                let v = u16::from_le_bytes(
                    self.panel.eup[base + off..base + off + 2]
                        .try_into()
                        .unwrap(),
                );
                self.hw_knob(
                    ui,
                    c,
                    x,
                    y,
                    if y == 848. { 28. } else { 23. },
                    label,
                    key,
                    deck,
                    v,
                );
            }
            self.hw_button(ui, c, x - 26., 904., 52., 23., "CUE", 0x5020, deck, ORANGE);
            c.text(x, 952., &deck.to_string(), 28., WHITE);
            let v = u16::from_le_bytes(
                self.panel.sub[14 + (deck - 1) * 4..16 + (deck - 1) * 4]
                    .try_into()
                    .unwrap(),
            );
            c.line([x, 988.], [x, 1120.], C::BLACK, 8.);
            for n in 0..11 {
                let y = 988. + n as f32 * 13.2;
                c.line([x - 27., y], [x - 4., y], SILVER, 1.);
                c.line([x + 4., y], [x + 27., y], SILVER, 1.);
            }
            let y = 1120. - v as f32 / 1023. * 132.;
            c.box_(x - 26., y - 8., 52., 16., 2., C::from_gray(37), SILVER);
            c.line([x - 24., y], [x + 24., y], WHITE, 3.);
            let response = ui.interact(
                c.rect(x - 30., 980., 60., 150.),
                ui.id().with(("channel-fader", deck)),
                Sense::click_and_drag(),
            );
            if response.is_pointer_button_down_on() {
                if let Some(p) = response.interact_pointer_pos() {
                    let raw = ((c.p(0., 1120.).y - p.y) / (132. * c.scale) * 1023.).clamp(0., 1023.)
                        as u16;
                    self.panel.analog(0x501e, deck, raw);
                    self.send();
                }
            }
        }
        c.box_(943., 581., 99., 269., 1., C::from_gray(9), C::from_gray(53));
        c.text(991., 598., "CH 1       MASTER       CH 2", 8., WHITE);
        for x in [956., 991., 1025.] {
            for n in 0..15 {
                let y = 627. + n as f32 * 14.;
                let color = if n < 3 {
                    C::from_rgb(62, 17, 17)
                } else if n < 8 {
                    C::from_rgb(56, 47, 13)
                } else {
                    C::from_rgb(18, 45, 23)
                };
                c.box_(x - 2., y, 4., 8., 0., color, color);
            }
        }
        let master = u16::from_le_bytes(self.panel.eup[74..76].try_into().unwrap());
        self.hw_knob(ui, c, 991., 540., 25., "MASTER LEVEL", 0x4403, 0, master);
        self.hw_button(ui, c, 965., 904., 52., 23., "CUE", 0x4407, 0, ORANGE);
        c.text(991., 952., "MASTER", 11., WHITE);
        c.text(991., 1020., "CROSSFADER CURVE", 10., WHITE);
        c.box_(966., 1032., 52., 35., 3., C::BLACK, SILVER);
        c.box_(985., 1039., 14., 21., 1., C::from_gray(66), SILVER);
        c.text(991., 1155., "Pioneer DJ", 29., WHITE);
        let cross = u16::from_le_bytes(self.panel.sub[22..24].try_into().unwrap());
        let x = 928. + cross as f32 / 1023. * 128.;
        c.line([922., 1218.], [1062., 1218.], C::BLACK, 8.);
        for n in 0..13 {
            let x = 928. + n as f32 * 10.7;
            c.line([x, 1186.], [x, 1208.], SILVER, 1.);
            c.line([x, 1228.], [x, 1250.], SILVER, 1.);
        }
        c.box_(x - 8., 1190., 16., 55., 2., C::from_gray(38), SILVER);
        c.line([x, 1194.], [x, 1241.], WHITE, 3.);
        let response = ui.interact(
            c.rect(920., 1185., 145., 65.),
            ui.id().with("crossfader"),
            Sense::click_and_drag(),
        );
        if response.is_pointer_button_down_on() {
            if let Some(p) = response.interact_pointer_pos() {
                let value =
                    ((p.x - c.p(928., 0.).x) / (128. * c.scale) * 1023.).clamp(0., 1023.) as u16;
                self.panel.analog(0x6017, 0, value);
                self.send();
            }
        }
        // Left utility strip and the right Beat FX strip.
        for (y, label) in [
            (524., "DECK 1 · LINE · PHONO"),
            (589., "AUX · LINE / PORTABLE"),
        ] {
            c.text(774., y, label, 9., WHITE);
            c.box_(755., y + 12., 39., 20., 2., C::BLACK, SILVER);
        }
        c.knob(774., 687., 23., 0.5);
        c.text(774., 656., "TRIM", 11., WHITE);
        for (i, label) in ["SPACE", "DUB ECHO", "SWEEP", "NOISE", "CRUSH", "FILTER"]
            .into_iter()
            .enumerate()
        {
            c.box_(
                711. + (i % 3) as f32 * 47.,
                813. + (i / 3) as f32 * 54.,
                32.,
                22.,
                1.,
                C::BLACK,
                SILVER,
            );
            c.text(
                727. + (i % 3) as f32 * 47.,
                824. + (i / 3) as f32 * 54.,
                label,
                7.,
                WHITE,
            );
        }
        c.text(774., 851., "SOUND COLOR FX", 11., WHITE);
        c.knob(774., 942., 22., 0.5);
        c.text(774., 911., "PARAMETER", 11., WHITE);
        c.text(774., 989., "HEADPHONES", 11., WHITE);
        for (y, label, key, off) in [(1059., "MIXING", 0x4405, 82), (1132., "LEVEL", 0x4406, 80)] {
            let v = u16::from_le_bytes(self.panel.eup[off..off + 2].try_into().unwrap());
            self.hw_knob(ui, c, 774., y, 22., label, key, 0, v);
        }
        c.text(1208., 569., "BOOTH MONITOR", 11., WHITE);
        c.knob(1208., 603., 23., 0.5);
        c.text(1208., 654., "BEAT FX", 16., WHITE);
        c.text(1208., 670., "BEAT", 10., WHITE);
        self.hw_button(ui, c, 1170., 680., 32., 23., "◀", 0x4490, 0, WHITE);
        self.hw_button(ui, c, 1217., 680., 32., 23., "▶", 0x4491, 0, WHITE);
        self.hw_round(ui, c, 1208., 738., 20., "TAP", 0x4492, 0, GREEN);
        c.text(1208., 785., "FILTER   FLANGER", 9., WHITE);
        c.text(1208., 799., "TRANS     PHASER", 9., WHITE);
        c.knob(1208., 826., 23., 0.5);
        c.text(1208., 874., "AUX   1   2   MASTER", 9., WHITE);
        c.knob(1208., 899., 22., 0.5);
        c.knob(1208., 974., 21., 0.5);
        c.text(1208., 946., "TIME", 11., WHITE);
        c.knob(1208., 1066., 22., 0.5);
        c.text(1208., 1036., "LEVEL / DEPTH", 11., WHITE);
        c.text(1208., 1122., "ON / OFF", 11., WHITE);
        self.hw_round(ui, c, 1208., 1158., 26., "", 0x448d, 0, BLUE);
        c.text(1208., 1230., "ALL-IN-ONE DJ SYSTEM", 8., WHITE);
        c.text(1208., 1248., "XDJ-RX3", 20., WHITE);
    }
}
