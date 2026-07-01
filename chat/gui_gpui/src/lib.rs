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
        if cap.read(&mut mat).unwrap_or(false) && !mat.empty() {
            let sz = mat.size().unwrap();
            *w.lock().unwrap() = sz.width as u32;
            *h.lock().unwrap() = sz.height as u32;
            *frame.lock().unwrap() = Some(mat.data_bytes().unwrap().to_vec());
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
    let client = reqwest::blocking::Client::new();
    let llama_url = "http://127.0.0.1:19080";

    *status.lock().unwrap() = "就绪".to_string();

    while *running.lock().unwrap() {
        let mut i = { let mut guard = idx.lock().unwrap(); *guard += 1; *guard };

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
                    if let Some(json_str) = line.strip_prefix("data: ") {
                        if json_str == "[DONE]" { continue; }
                        if let Ok(val) = serde_json::from_str::<serde_json::Value>(json_str) {
                            if let Some(true) = val.get("is_listen").and_then(|v| v.as_bool()) {
                                continue;
                            }
                            if let Some(content) = val.get("content").and_then(|v| v.as_str()) {
                                if !content.is_empty() && content != "__IS_LISTEN__" && content != "__END_OF_TURN__" {
                                    println!("[AI] {}", content);
                                    *ai_text.lock().unwrap() = content.to_string();
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

        // cleanup old files
        if i > 10 {
            let _ = std::fs::remove_file(format!("/tmp/f_{}.jpg", i - 10));
            let _ = std::fs::remove_file(format!("/tmp/m_{}.wav", i - 10));
        }

        thread::sleep(Duration::from_millis(500));
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
    let app = App {
        frame: Arc::new(Mutex::new(None)),
        w: Arc::new(Mutex::new(640)),
        h: Arc::new(Mutex::new(480)),
        ai_text: Arc::new(Mutex::new(String::new())),
        status: Arc::new(Mutex::new("初始化中...".to_string())),
        running: Arc::new(Mutex::new(true)),
        idx: Arc::new(Mutex::new(0)),
    };

    eprintln!("[create] 启动摄像头线程...");
    { let f = app.frame.clone(); let w = app.w.clone(); let h = app.h.clone();
      thread::spawn(move || {
          eprintln!("[camera] 线程启动");
          camera_loop(f, w, h);
      }); }
    eprintln!("[create] 启动推理线程...");

    { let f = app.frame.clone(); let w = app.w.clone(); let h = app.h.clone();
      let ai = app.ai_text.clone(); let st = app.status.clone(); let run = app.running.clone();
      let idx = app.idx.clone();
      thread::spawn(move || {
          eprintln!("[ai] 线程启动");
          ai_loop(f, w, h, ai, st, run, idx);
      }); }
    eprintln!("[create] 线程已启动，返回 app");

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

    // wait for model
    loop { let st = app.status.lock().unwrap();
          if st.as_str() == "就绪" || st.contains("失败") { break; }
          thread::sleep(Duration::from_millis(100)); }

    eprintln!("[gui_run] 创建 SDL 窗口");
    let sdl = match sdl2::init() { Ok(s) => s, Err(e) => { eprintln!("sdl: {}", e); return -1; } };
    let video = match sdl.video() { Ok(v) => v, Err(e) => { eprintln!("video: {}", e); return -1; } };
    let win = match video.window("Friday 监控", 1280, 960).position_centered().resizable().build() {
        Ok(w) => w, Err(e) => { eprintln!("window: {}", e); return -1; }
    };
    let mut canvas = match win.into_canvas().build() {
        Ok(c) => c, Err(e) => { eprintln!("canvas: {}", e); return -1; }
    };
    let tc = canvas.texture_creator();
    let mut events = sdl.event_pump().expect("events");
    let mut tex: Option<sdl2::render::Texture> = None;

    eprintln!("[gui_run] 进入主循环");
    let mut frame_count = 0u64;

    loop {
        frame_count += 1;
        if frame_count % 100 == 0 {
            eprintln!("[gui_run] frame {}", frame_count);
        }

        for e in events.poll_iter() {
            match e {
                sdl2::event::Event::Quit { .. } => { eprintln!("[gui_run] quit"); return 0; }
                sdl2::event::Event::KeyDown { keycode: Some(sdl2::keyboard::Keycode::Escape), .. } => { eprintln!("[gui_run] esc"); return 0; }
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
