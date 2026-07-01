use std::ffi::{c_void, c_char, CString};
use std::os::raw::c_int;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

struct App {
    frame: Arc<Mutex<Option<Vec<u8>>>>,
    w: Arc<Mutex<u32>>,
    h: Arc<Mutex<u32>>,
    ai_text: Arc<Mutex<String>>,
    status: Arc<Mutex<String>>,
    running: Arc<Mutex<bool>>,
    idx: Arc<Mutex<u64>>,
}

fn camera_loop(frame: Arc<Mutex<Option<Vec<u8>>>>, w: Arc<Mutex<u32>>, h: Arc<Mutex<u32>>) {
    use opencv::prelude::*;
    use opencv::videoio::VideoCapture;

    let mut cap = match VideoCapture::new(0, opencv::videoio::CAP_ANY) {
        Ok(c) => c,
        Err(e) => { eprintln!("[cam] {}", e); return; }
    };
    let _ = cap.set(opencv::videoio::CAP_PROP_FRAME_WIDTH, 640.0);
    let _ = cap.set(opencv::videoio::CAP_PROP_FRAME_HEIGHT, 480.0);

    let mut mat = Mat::default();
    loop {
        match cap.read(&mut mat) {
            Ok(true) if !mat.empty() => {
                if let Ok(sz) = mat.size() {
                    *w.lock().unwrap_or_else(|e| e.into_inner()) = sz.width as u32;
                    *h.lock().unwrap_or_else(|e| e.into_inner()) = sz.height as u32;
                    if let Ok(data) = mat.data_bytes() {
                        *frame.lock().unwrap_or_else(|e| e.into_inner()) = Some(data.to_vec());
                    }
                }
            }
            _ => {}
        }
        thread::sleep(Duration::from_millis(33));
    }
}

fn ai_loop(
    frame: Arc<Mutex<Option<Vec<u8>>>>,
    frame_w: Arc<Mutex<u32>>,
    frame_h: Arc<Mutex<u32>>,
    ai_text: Arc<Mutex<String>>,
    status: Arc<Mutex<String>>,
    running: Arc<Mutex<bool>>,
    idx: Arc<Mutex<u64>>,
) {
    eprintln!("[ai] setting status to ready");
    *status.lock().unwrap_or_else(|e| e.into_inner()) = "就绪".to_string();
    eprintln!("[ai] status set, entering loop");

    let client = reqwest::blocking::Client::new();
    let llama_url = "http://127.0.0.1:19080";

    let mut is_listening = true;

    while *running.lock().unwrap() {
        let mut i = { let mut g = idx.lock().unwrap(); *g += 1; *g };

        if is_listening {
            // LISTEN: 只发 decode
            if let Ok(resp) = client.post(format!("{}/v1/stream/decode", llama_url))
                .json(&serde_json::json!({"stream": true}))
                .timeout(Duration::from_secs(30))
                .send()
            {
                if let Ok(text) = resp.text() {
                    for line in text.lines() {
                        if let Some(s) = line.strip_prefix("data: ") {
                            if s == "[DONE]" { continue; }
                            if let Ok(v) = serde_json::from_str::<serde_json::Value>(s) {
                                if v.get("is_listen").and_then(|x| x.as_bool()).unwrap_or(false) { continue; }
                                if let Some(c) = v.get("content").and_then(|x| x.as_str()) {
                                    if !c.is_empty() && c != "__IS_LISTEN__" && c != "__END_OF_TURN__" {
                                        eprintln!("[AI] {}", c);
                                        *ai_text.lock().unwrap() = c.to_string();
                                        is_listening = false;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // SPEAK: 抓帧 + prefill + decode
            let data = frame.lock().unwrap().clone();
            let w = *frame_w.lock().unwrap();
            let h = *frame_h.lock().unwrap();

            let img_path = format!("/tmp/f_{}.jpg", i);
            if let Some(ref bgr) = data {
                if w > 0 && h > 0 && bgr.len() >= (w * h * 3) as usize {
                    save_bgr_jpeg(bgr, w, h, &img_path);
                }
            }

            let wav_path = format!("/tmp/m_{}.wav", i);
            record_mic(&wav_path);

            *status.lock().unwrap() = "推理中...".to_string();

            // prefill
            let _ = client.post(format!("{}/v1/stream/prefill", llama_url))
                .json(&serde_json::json!({
                    "audio_path_prefix": wav_path,
                    "img_path_prefix": img_path,
                    "cnt": i,
                }))
                .timeout(Duration::from_secs(10))
                .send();

            // decode
            if let Ok(resp) = client.post(format!("{}/v1/stream/decode", llama_url))
                .json(&serde_json::json!({"debug_dir":"/tmp/omni_out2","stream":true}))
                .timeout(Duration::from_secs(120))
                .send()
            {
                if let Ok(text) = resp.text() {
                    for line in text.lines() {
                        if let Some(s) = line.strip_prefix("data: ") {
                            if s == "[DONE]" { continue; }
                            if let Ok(v) = serde_json::from_str::<serde_json::Value>(s) {
                                if v.get("is_listen").and_then(|x| x.as_bool()).unwrap_or(false) {
                                    is_listening = true;
                                    continue;
                                }
                                if let Some(c) = v.get("content").and_then(|x| x.as_str()) {
                                    if !c.is_empty() && c != "__IS_LISTEN__" && c != "__END_OF_TURN__" {
                                        eprintln!("[AI] {}", c);
                                        *ai_text.lock().unwrap() = c.to_string();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // TTS
            let _ = std::process::Command::new("sh")
                .arg("-c")
                .arg("ls -t /tmp/omni_out2/round_*/tts_wav/wav_*.wav 2>/dev/null|head -1|xargs -r aplay -D plughw:0,3 -q 2>/dev/null &")
                .status();

            *status.lock().unwrap() = "就绪".to_string();

            if i > 10 {
                let _ = std::fs::remove_file(format!("/tmp/f_{}.jpg", i - 10));
                let _ = std::fs::remove_file(format!("/tmp/m_{}.wav", i - 10));
            }
        }

        thread::sleep(Duration::from_millis(200));
    }
}

fn save_bgr_jpeg(data: &[u8], w: u32, h: u32, path: &str) {
    let row_bytes = w * 3;
    let padded_row = ((row_bytes + 3) / 4) * 4;
    let data_size = padded_row * h;
    let file_size: u32 = 54 + data_size;
    let mut bmp = Vec::with_capacity(file_size as usize);
    bmp.extend_from_slice(b"BM");
    bmp.extend_from_slice(&file_size.to_le_bytes());
    bmp.extend(&[0u8; 4]);
    bmp.extend_from_slice(&54u32.to_le_bytes());
    bmp.extend_from_slice(&40u32.to_le_bytes());
    bmp.extend_from_slice(&w.to_le_bytes());
    bmp.extend_from_slice(&h.to_le_bytes());
    bmp.extend_from_slice(&1u16.to_le_bytes());
    bmp.extend_from_slice(&24u16.to_le_bytes());
    bmp.extend_from_slice(&0u32.to_le_bytes());
    bmp.extend_from_slice(&data_size.to_le_bytes());
    bmp.extend(&[0u8; 24]);
    let padding = padded_row - row_bytes;
    for y in (0..h).rev() {
        let start = (y * row_bytes) as usize;
        bmp.extend_from_slice(&data[start..start + row_bytes as usize]);
        if padding > 0 { bmp.extend(vec![0u8; padding as usize]); }
    }
    let bmp_path = format!("{}.bmp", path);
    let _ = std::fs::write(&bmp_path, &bmp);
    let _ = std::process::Command::new("sh")
        .arg("-c")
        .arg(format!("ffmpeg -y -i {} {} 2>/dev/null && rm -f {}", bmp_path, path, bmp_path))
        .status();
}

fn record_mic(path: &str) {
    for dev in &["plughw:2,0", "hw:1,0", "hw:3,0", "default"] {
        let cmd = format!("ffmpeg -f alsa -ac 1 -ar 16000 -i {} -t 1 -y {} 2>/dev/null", dev, path);
        if std::process::Command::new("sh").arg("-c").arg(&cmd).status().map(|s| s.success()).unwrap_or(false) {
            return;
        }
    }
    let _ = std::fs::write(path, create_silence_wav());
}

fn create_silence_wav() -> Vec<u8> {
    let data_size = 32000usize;
    let mut wav = Vec::with_capacity(44 + data_size);
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36 + data_size as u32).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&16000u32.to_le_bytes());
    wav.extend_from_slice(&32000u32.to_le_bytes());
    wav.extend_from_slice(&2u16.to_le_bytes());
    wav.extend_from_slice(&16u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&(data_size as u32).to_le_bytes());
    wav.extend(vec![0u8; data_size]);
    wav
}

#[no_mangle]
pub extern "C" fn gui_app_create(_cfg: *const c_char) -> *mut c_void {
    eprintln!("[create] start");

    let app = App {
        frame: Arc::new(Mutex::new(None)),
        w: Arc::new(Mutex::new(640)),
        h: Arc::new(Mutex::new(480)),
        ai_text: Arc::new(Mutex::new(String::new())),
        status: Arc::new(Mutex::new("初始化中...".to_string())),
        running: Arc::new(Mutex::new(true)),
        idx: Arc::new(Mutex::new(0)),
    };

    eprintln!("[create] spawning camera thread...");
    { let f = app.frame.clone(); let w = app.w.clone(); let h = app.h.clone();
      thread::spawn(move || {
          eprintln!("[camera] thread start");
          camera_loop(f, w, h);
          eprintln!("[camera] thread end");
      }); }
    eprintln!("[create] camera thread spawned");

    eprintln!("[create] spawning ai thread...");
    { let f = app.frame.clone(); let w = app.w.clone(); let h = app.h.clone();
      let ai = app.ai_text.clone(); let st = app.status.clone(); let run = app.running.clone();
      let idx = app.idx.clone();
      thread::spawn(move || {
          eprintln!("[ai] thread start");
          ai_loop(f, w, h, ai, st, run, idx);
          eprintln!("[ai] thread end");
      }); }
    eprintln!("[create] ai thread spawned");

    eprintln!("[create] returning app");
    Box::into_raw(Box::new(app)) as *mut c_void
}

#[no_mangle]
pub extern "C" fn gui_app_free(app: *mut c_void) {
    if !app.is_null() {
        *unsafe { &*(app as *const App) }.running.lock().unwrap() = false;
        unsafe { drop(Box::from_raw(app as *mut App)) };
    }
}

#[no_mangle]
pub extern "C" fn gui_run(app: *mut c_void) -> c_int {
    let app = unsafe { &*(app as *const App) };

    loop { let st = app.status.lock().unwrap_or_else(|e| e.into_inner()).clone();
          if st == "就绪" || st.contains("失败") { break; }
          thread::sleep(Duration::from_millis(100)); }

    let sdl = sdl2::init().expect("sdl");
    let video = sdl.video().expect("video");
    let win = video.window("Friday 监控", 1280, 960)
        .position_centered().resizable().build().expect("window");
    let mut canvas = win.into_canvas().build().expect("canvas");
    let tc = canvas.texture_creator();
    let mut events = sdl.event_pump().expect("events");
    let mut tex: Option<sdl2::render::Texture> = None;

    loop {
        for e in events.poll_iter() {
            match e {
                sdl2::event::Event::Quit { .. } => return 0,
                sdl2::event::Event::KeyDown { keycode: Some(sdl2::keyboard::Keycode::Escape), .. } => return 0,
                _ => {}
            }
        }

        if let Some(data) = app.frame.lock().unwrap().take() {
            let w = *app.w.lock().unwrap();
            let h = *app.h.lock().unwrap();
            if w > 0 && h > 0 && data.len() >= (w * h * 3) as usize {
                let need_new = tex.as_ref().map_or(true, |t| {
                    let q = t.query(); q.width != w || q.height != h
                });
                if need_new {
                    tex = tc.create_texture_streaming(sdl2::pixels::PixelFormatEnum::BGR24, w, h).ok();
                }
                if let Some(ref mut t) = tex {
                    t.update(None, &data, (w * 3) as usize).ok();
                    canvas.clear();
                    let dst = sdl2::rect::Rect::new(0, 0, 1280, 960);
                    let _ = canvas.copy(t, None, Some(dst));
                }
            }
        }
        canvas.present();
        thread::sleep(Duration::from_millis(33));
    }
}

#[no_mangle]
pub extern "C" fn gui_stop(app: *mut c_void) {
    if !app.is_null() {
        *unsafe { &*(app as *const App) }.running.lock().unwrap() = false;
    }
}

#[no_mangle]
pub extern "C" fn gui_stream_delta(_: *mut c_void, _: *const c_char) {}
#[no_mangle]
pub extern "C" fn gui_set_status(_: *mut c_void, _: *const c_char) {}
#[no_mangle]
pub extern "C" fn gui_free_string(s: *mut c_char) {
    if !s.is_null() { unsafe { drop(CString::from_raw(s)) }; }
}
