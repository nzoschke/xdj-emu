//! XDJ-RX3's two independent main-panel receive channels. Layout and CRC are
//! verified against stock 1.20's validators; see docs/xdj-rx3-panel.md.
//! The ready-line jog microcontrollers use a separate protocol.

#[derive(Clone)]
pub struct Rx3Panel {
    pub eup: [u8; 96],
    pub sub: [u8; 24],
}

impl Default for Rx3Panel {
    fn default() -> Self {
        let mut s = Self::diagnostic();
        for base in [4, 20] {
            put16(&mut s.eup, base, 0x1ff8);
            put16(&mut s.eup, base + 6, 0x1ff8);
        }
        for base in [36, 52] {
            for offset in [0, 2, 4, 6, 8] {
                put16(&mut s.eup, base + offset, 512);
            }
        }
        put16(&mut s.eup, 74, 768);
        put16(&mut s.eup, 80, 512);
        put16(&mut s.eup, 82, 512);
        for base in [12, 16] {
            put16(&mut s.sub, base + 2, 1023);
        }
        put16(&mut s.sub, 22, 512);
        s
    }
}

impl Rx3Panel {
    pub const fn diagnostic() -> Self {
        Self {
            eup: [0; 96],
            sub: [0; 24],
        }
    }

    /// One firmware key state. Deck is 1 or 2; global controls use 0.
    /// Returns false for an unmapped key or channel.
    pub fn key(&mut self, key: u16, deck: usize, down: bool) -> bool {
        let (sub, offset, bit) = if deck == 0 {
            let (offset, bit) = match key {
                0x0201 => (92, 7),
                0x0202 => (92, 6),
                0x0203 => (92, 5),
                0x0204 => (92, 4),
                0x0205 => (92, 3),
                0x0206 => (92, 2),
                0x420d => (94, 0),
                0x420e => (93, 7),
                0x0210 => (93, 2),
                0x420f => (93, 3),
                0x420c => (93, 4),
                0x448d => (91, 2),
                0x4490 => (90, 1),
                0x4491 => (90, 0),
                0x4492 => (91, 3),
                0x4407 => (88, 7),
                _ => return false,
            };
            (false, offset, bit)
        } else if deck <= 2 {
            match key {
                0x4101 => (true, 7 + (deck - 1) * 4, 6),
                0x4102 => (true, 7 + (deck - 1) * 4, 7),
                0x4113 => (true, 7 + (deck - 1) * 4, 3),
                0x4114 => (true, 7 + (deck - 1) * 4, 2),
                0x4115 => (true, 7 + (deck - 1) * 4, 1),
                0x4116 => (true, 7 + (deck - 1) * 4, 0),
                0x4117..=0x411e => (true, 6 + (deck - 1) * 4, (key - 0x4117) as u8),
                0x4311 => (false, 93, if deck == 1 { 6 } else { 5 }),
                0x8002 => (false, 94, if deck == 1 { 4 } else { 3 }),
                0x5020 => (false, 91, if deck == 1 { 1 } else { 0 }),
                _ => {
                    let (offset, bit) = match key {
                        0x4306 => (16, 0),
                        0x4110 => (17, 0),
                        0x4121 => (17, 2),
                        0x410b => (17, 3),
                        0x411f => (17, 4),
                        0x4120 => (17, 5),
                        0x4214 => (17, 6),
                        0x4215 => (17, 7),
                        0x4103 => (18, 0),
                        0x410f => (18, 1),
                        0x4108 => (18, 2),
                        0x4107 => (18, 3),
                        0x4111 => (18, 4),
                        0x4112 => (18, 5),
                        0x4104 => (18, 6),
                        0x410e => (18, 7),
                        0x410d => (19, 0),
                        0x410c => (19, 1),
                        0x4125 => (19, 2),
                        0x4124 => (19, 3),
                        0x4322 => (19, 4),
                        0x4323 => (19, 5),
                        _ => return false,
                    };
                    (false, offset + (deck - 1) * 16, bit)
                }
            }
        } else {
            return false;
        };
        let byte = if sub {
            &mut self.sub[offset]
        } else {
            &mut self.eup[offset]
        };
        if down {
            *byte |= 1 << bit;
        } else {
            *byte &= !(1 << bit);
        }
        true
    }

    pub fn browse(&mut self, delta: i16) {
        let value = u16::from_le_bytes([self.eup[84], self.eup[85]]);
        put16(&mut self.eup, 84, value.wrapping_add_signed(delta));
    }

    /// Raw 10-bit analog control, clamped to the ADC range.
    pub fn analog(&mut self, key: u16, deck: usize, value: u16) -> bool {
        let value = value.min(1023);
        match (key, deck) {
            (0x501e, 1..=2) => put16(&mut self.sub, 14 + (deck - 1) * 4, value),
            (0x6017, 0) => put16(&mut self.sub, 22, value),
            (0x4403, 0) => put16(&mut self.eup, 74, value),
            (0x4406, 0) => put16(&mut self.eup, 80, value),
            (0x4405, 0) => put16(&mut self.eup, 82, value),
            (0x5019, 1..=2) => put16(&mut self.eup, 38 + (deck - 1) * 16, value),
            (0x501a, 1..=2) => put16(&mut self.eup, 36 + (deck - 1) * 16, value),
            (0x501b, 1..=2) => put16(&mut self.eup, 40 + (deck - 1) * 16, value),
            (0x501d, 1..=2) => put16(&mut self.eup, 44 + (deck - 1) * 16, value),
            (0x501c, 1..=2) => put16(&mut self.eup, 42 + (deck - 1) * 16, value),
            _ => return false,
        }
        true
    }

    pub fn eup_frame(&self) -> [u8; 100] {
        let mut frame = [0; 100];
        frame[..96].copy_from_slice(&self.eup);
        frame[98..].copy_from_slice(&checksum(&self.eup).to_le_bytes());
        frame
    }

    pub fn sub_frame(&self) -> [u8; 104] {
        let mut frame = [0; 104];
        frame[..24].copy_from_slice(&self.sub);
        frame[26..28].copy_from_slice(&checksum(&self.sub).to_le_bytes());
        frame
    }

    pub fn records(&self) -> Vec<u8> {
        let mut result = Vec::with_capacity(220);
        for (channel, bytes) in [
            (0, self.eup_frame().as_slice()),
            (1, self.sub_frame().as_slice()),
        ] {
            result.extend_from_slice(&[channel, 0]);
            result.extend_from_slice(&(bytes.len() as u16).to_le_bytes());
            result.extend_from_slice(&0u32.to_le_bytes());
            result.extend_from_slice(bytes);
        }
        result
    }
}

fn put16(data: &mut [u8], at: usize, value: u16) {
    data[at..at + 2].copy_from_slice(&value.to_le_bytes());
}

fn checksum(payload: &[u8]) -> u16 {
    let mut crc = 0xffff;
    for word in payload.as_chunks::<4>().0 {
        for &byte in word.iter().rev() {
            crc ^= u16::from(byte);
            for _ in 0..8 {
                crc = (crc >> 1) ^ if crc & 1 != 0 { 0x8408 } else { 0 };
            }
        }
    }
    crc ^ 0xffff
}

#[cfg(test)]
mod tests {
    use super::*;

    // Captured inputs accepted by the stock firmware's real validators.
    #[test]
    fn firmware_accepted_frames() {
        let mut s = Rx3Panel::diagnostic();
        assert_eq!(&s.eup_frame()[98..], &[0xae, 0xcb]);
        assert_eq!(&s.sub_frame()[26..28], &[0x08, 0xae]);
        assert!(s.key(0x410b, 2, true));
        assert_eq!(&s.eup_frame()[98..], &[0x1d, 0x2f]);
        assert_eq!(s.eup[33], 8);
        assert!(s.key(0x410b, 2, false));
        assert_eq!(&s.eup_frame()[98..], &[0xae, 0xcb]);
        assert!(s.key(0x4101, 1, true));
        assert_eq!(&s.sub_frame()[26..28], &[0xde, 0xc8]);
        assert!(!s.key(0x4101, 3, true));
    }
}
