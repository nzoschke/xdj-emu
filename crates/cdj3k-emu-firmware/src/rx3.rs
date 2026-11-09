//! XDJ-RX3 firmware updates.
//!
//! Unlike the CDJ-3000 family, an XDJ-RX3 `.UPD` is not a LUKS container. It
//! is an ISO 9660 image encrypted sector by sector with AES-256-CBC, followed
//! by a 16-byte trailer:
//!
//! * the key is the player's `aes256.key`: its first line, cut to 31 bytes
//!   and NUL-padded to 32 (the player reads it with `fgets` into a 32-byte
//!   buffer);
//! * each 512-byte sector `n` is encrypted on its own, with the IV set to `n`
//!   as a little-endian u32 followed by 12 zero bytes;
//! * the body must be a whole number of sectors; the trailer is ignored.
//!
//! A decrypted image is recognised by the ISO 9660 `CD001` signature of the
//! primary volume descriptor at byte 32769.

use std::fs::File;
use std::io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

use aes::cipher::{BlockDecryptMut, KeyIvInit};

type Aes256CbcDec = cbc::Decryptor<aes::Aes256>;

/// Bytes after the encrypted body that are not part of the image.
pub const TRAILER_BYTES: u64 = 16;
/// Size of an independently encrypted sector.
pub const SECTOR_BYTES: usize = 512;
/// Offset of the ISO 9660 primary volume descriptor's signature.
const ISO_SIGNATURE_OFFSET: u64 = 32769;

/// The 32 bytes the player derives from its key file.
pub fn effective_key(raw: &[u8]) -> [u8; 32] {
    let first_line = raw
        .split(|b| *b == b'\n' || *b == b'\r')
        .next()
        .unwrap_or(&[]);
    let mut key = [0u8; 32];
    let n = first_line.len().min(31);
    key[..n].copy_from_slice(&first_line[..n]);
    key
}

#[derive(Debug)]
pub enum Rx3Error {
    Io(io::Error),
    /// The body (file length minus trailer) is not a whole number of sectors.
    NotSectorAligned {
        body_len: u64,
    },
    /// Decryption succeeded mechanically but the result is not an ISO 9660
    /// image: almost always the wrong key, or a file that is not an RX3 UPD.
    NotIso,
}

impl std::fmt::Display for Rx3Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "{e}"),
            Self::NotSectorAligned { body_len } => write!(
                f,
                "not an XDJ-RX3 update: its body ({body_len} bytes) is not a whole number of \
                 {SECTOR_BYTES}-byte sectors"
            ),
            Self::NotIso => write!(
                f,
                "the update did not decrypt to an ISO image: check that the key is the \
                 XDJ-RX3 aes256.key"
            ),
        }
    }
}

impl std::error::Error for Rx3Error {}

impl From<io::Error> for Rx3Error {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

fn decrypt_sector(key: &[u8; 32], index: u32, sector: &mut [u8]) {
    let mut iv = [0u8; 16];
    iv[..4].copy_from_slice(&index.to_le_bytes());
    let dec = Aes256CbcDec::new(key.into(), &iv.into());
    // Sectors are whole AES blocks, so no padding is involved.
    let blocks = sector.len() / 16;
    let (chunks, _) = sector.as_chunks_mut::<16>();
    let mut dec = dec;
    for block in chunks.iter_mut().take(blocks) {
        dec.decrypt_block_mut(block.into());
    }
}

fn body_len<R: Seek>(r: &mut R) -> Result<u64, Rx3Error> {
    let len = r.seek(SeekFrom::End(0))?;
    let body = len.saturating_sub(TRAILER_BYTES);
    if body == 0 || body % SECTOR_BYTES as u64 != 0 {
        return Err(Rx3Error::NotSectorAligned { body_len: body });
    }
    Ok(body)
}

/// Decrypts only the sector holding the ISO signature, to check a key (or
/// recognise an RX3 update) without writing anything.
pub fn probe<R: Read + Seek>(r: &mut R, key: &[u8; 32]) -> Result<bool, Rx3Error> {
    let body = body_len(r)?;
    let index = ISO_SIGNATURE_OFFSET / SECTOR_BYTES as u64;
    if (index + 1) * SECTOR_BYTES as u64 > body {
        return Ok(false);
    }
    let mut sector = [0u8; SECTOR_BYTES];
    r.seek(SeekFrom::Start(index * SECTOR_BYTES as u64))?;
    r.read_exact(&mut sector)?;
    decrypt_sector(key, index as u32, &mut sector);
    let at = (ISO_SIGNATURE_OFFSET % SECTOR_BYTES as u64) as usize;
    Ok(&sector[at..at + 5] == b"CD001")
}

/// Decrypts the XDJ-RX3 update at `upd_path` with the raw key file contents
/// `raw_key` and writes the ISO image to `out_path`. The key is checked
/// before anything is written.
pub fn decrypt_upd(upd_path: &Path, raw_key: &[u8], out_path: &Path) -> Result<(), Rx3Error> {
    let key = effective_key(raw_key);
    let mut file = File::open(upd_path)?;
    if !probe(&mut file, &key)? {
        return Err(Rx3Error::NotIso);
    }
    let body = body_len(&mut file)?;
    file.seek(SeekFrom::Start(0))?;
    let mut reader = BufReader::with_capacity(1 << 20, file).take(body);
    let tmp = out_path.with_extension("partial");
    let mut out = BufWriter::with_capacity(1 << 20, File::create(&tmp)?);
    let mut sector = [0u8; SECTOR_BYTES];
    let mut index: u32 = 0;
    loop {
        match reader.read_exact(&mut sector) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e.into()),
        }
        decrypt_sector(&key, index, &mut sector);
        out.write_all(&sector)?;
        index = index.wrapping_add(1);
    }
    out.into_inner().map_err(|e| e.into_error())?.sync_all()?;
    std::fs::rename(&tmp, out_path)?;
    Ok(())
}

/// True when `upd_path` is an XDJ-RX3 update that `raw_key` opens.
pub fn key_opens(upd_path: &Path, raw_key: &[u8]) -> Result<bool, Rx3Error> {
    let mut file = File::open(upd_path)?;
    probe(&mut file, &effective_key(raw_key))
}

#[cfg(test)]
mod tests {
    use super::*;
    use aes::cipher::BlockEncryptMut;

    type Aes256CbcEnc = cbc::Encryptor<aes::Aes256>;

    fn encrypt_image(key: &[u8; 32], plain: &[u8]) -> Vec<u8> {
        let mut out = plain.to_vec();
        for (i, sector) in out.chunks_mut(SECTOR_BYTES).enumerate() {
            let mut iv = [0u8; 16];
            iv[..4].copy_from_slice(&(i as u32).to_le_bytes());
            let mut enc = Aes256CbcEnc::new(key.into(), &iv.into());
            let (chunks, _) = sector.as_chunks_mut::<16>();
            for block in chunks {
                enc.encrypt_block_mut(block.into());
            }
        }
        out.extend_from_slice(&[0xAA; TRAILER_BYTES as usize]);
        out
    }

    fn fake_iso() -> Vec<u8> {
        let mut iso = vec![0u8; 40 * 1024];
        iso[32768] = 1;
        iso[32769..32774].copy_from_slice(b"CD001");
        for (i, b) in iso.iter_mut().enumerate().skip(34000) {
            *b = (i % 251) as u8;
        }
        iso
    }

    #[test]
    fn effective_key_takes_first_line_31_bytes_nul_padded() {
        let k = effective_key(b"0123456789abcdef0123456789abcdefXYZ\nsecond line\n");
        assert_eq!(&k[..31], b"0123456789abcdef0123456789abcde");
        assert_eq!(k[31], 0);
        let short = effective_key(b"abc\r\n");
        assert_eq!(&short[..3], b"abc");
        assert!(short[3..].iter().all(|b| *b == 0));
        assert_eq!(effective_key(b""), [0u8; 32]);
    }

    #[test]
    fn round_trip_decrypts_and_drops_trailer() {
        let raw_key = b"not-the-real-key-just-a-test-value\n";
        let iso = fake_iso();
        let upd = encrypt_image(&effective_key(raw_key), &iso);
        let dir = tempdir();
        let upd_path = dir.join("XDJRX3.UPD");
        let out_path = dir.join("out.iso");
        std::fs::write(&upd_path, &upd).unwrap();
        assert!(key_opens(&upd_path, raw_key).unwrap());
        decrypt_upd(&upd_path, raw_key, &out_path).unwrap();
        assert_eq!(std::fs::read(&out_path).unwrap(), iso);
    }

    #[test]
    fn wrong_key_is_rejected_before_writing() {
        let iso = fake_iso();
        let upd = encrypt_image(&effective_key(b"right key"), &iso);
        let dir = tempdir();
        let upd_path = dir.join("XDJRX3.UPD");
        let out_path = dir.join("out.iso");
        std::fs::write(&upd_path, &upd).unwrap();
        assert!(!key_opens(&upd_path, b"wrong key").unwrap());
        assert!(matches!(
            decrypt_upd(&upd_path, b"wrong key", &out_path),
            Err(Rx3Error::NotIso)
        ));
        assert!(!out_path.exists());
    }

    #[test]
    fn unaligned_file_is_not_an_rx3_update() {
        let dir = tempdir();
        let p = dir.join("odd.UPD");
        std::fs::write(&p, vec![0u8; 1000]).unwrap();
        assert!(matches!(
            key_opens(&p, b"k"),
            Err(Rx3Error::NotSectorAligned { .. })
        ));
    }

    fn tempdir() -> std::path::PathBuf {
        let mut bytes = [0u8; 8];
        getrandom::fill(&mut bytes).unwrap();
        let p = std::env::temp_dir().join(format!("rx3-test-{}", hex::encode(bytes)));
        std::fs::create_dir_all(&p).unwrap();
        p
    }
}
