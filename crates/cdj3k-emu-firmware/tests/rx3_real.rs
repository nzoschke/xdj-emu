//! Decrypts a real XDJ-RX3 update. Ignored by default: it needs files this
//! repository does not ship. Run with
//! `RX3_UPD=<XDJRX3.UPD> RX3_KEY=<aes256.key> cargo test -p cdj3k-emu-firmware --test rx3_real -- --ignored`.

use std::path::PathBuf;

#[test]
#[ignore = "needs RX3_UPD and RX3_KEY"]
fn real_update_decrypts_to_iso() {
    let upd = PathBuf::from(std::env::var("RX3_UPD").expect("RX3_UPD"));
    let key = std::fs::read(std::env::var("RX3_KEY").expect("RX3_KEY")).unwrap();
    let out = std::env::temp_dir().join("rx3-real-test.iso");
    cdj3k_emu_firmware::rx3::decrypt_upd(&upd, &key, &out).unwrap();
    let iso = std::fs::read(&out).unwrap();
    assert_eq!(&iso[32769..32774], b"CD001");
    assert_eq!(iso.len() as u64, std::fs::metadata(&upd).unwrap().len() - 16);
    std::fs::remove_file(out).unwrap();
}
