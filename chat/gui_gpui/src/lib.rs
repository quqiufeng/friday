use std::ffi::{c_void, c_char, CStr, CString};
use std::os::raw::c_int;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

struct App {
    frame: Arc<Mutex<Option<Vec<u8>>>>,  // BGR24 raw data
    w: Arc<Mutex<u32>>,
    h: Arc<Mutex<u32>>,
}

// 摄像头线程 — 照着 gui3 的 ai_worker 里的摄像头部分
fn camera_loop(frame: Arc<Mutex<Option<Vec<u8>>>>, w: Arc<Mutex<u32>>, h: Arc<Mutex<u32>>) {
    use opencv::prelude::*;
    use opencv::videoio::VideoCapture;

    let mut cap = VideoCapture::new(0, opencv::videoio::CAP_ANY).expect("camera open failed");
    cap.set(opencv::videoio::CAP_PROP_FRAME_WIDTH, 640.0).ok();
    cap.set(opencv::videoio::CAP_PROP_FRAME_HEIGHT, 480.0).expect("set res failed");

    let mut mat = Mat::default();

    loop {
        if cap.read(&mut mat).unwrap_or(false) && !mat.empty() {
            let sz = mat.size().unwrap();
            let data = mat.data_bytes().unwrap().to_vec();
            *w.lock().unwrap() = sz.width as u32;
            *h.lock().unwrap() = sz.height as u32;
            *frame.lock().unwrap() = Some(data);
        }
        thread::sleep(Duration::from_millis(10));
    }
}

#[no_mangle]
pub extern "C" fn gui_app_create(_cfg: *const c_char) -> *mut c_void {
    let app = App {
        frame: Arc::new(Mutex::new(None)),
        w: Arc::new(Mutex::new(640)),
        h: Arc::new(Mutex::new(480)),
    };
    let f = app.frame.clone();
    let w = app.w.clone();
    let h = app.h.clone();
    thread::spawn(move || camera_loop(f, w, h));
    Box::into_raw(Box::new(app)) as *mut c_void
}

#[no_mangle]
pub extern "C" fn gui_app_free(app: *mut c_void) {
    if !app.is_null() { unsafe { drop(Box::from_raw(app as *mut App)) }; }
}

#[no_mangle]
pub extern "C" fn gui_run(app: *mut c_void) -> c_int {
    let app = unsafe { &*(app as *const App) };

    let sdl = sdl2::init().expect("sdl init");
    let video = sdl.video().expect("sdl video");
    let win = video.window("Friday", 960, 640)
        .position_centered().resizable().build().expect("window");
    let mut canvas = win.into_canvas().build().expect("canvas");
    let tc = canvas.texture_creator();

    let mut tex = tc.create_texture_streaming(
        sdl2::pixels::PixelFormatEnum::BGR24, 640, 480
    ).expect("texture");

    let mut events = sdl.event_pump().expect("events");

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
            tex.update(None, &data, (w * 3) as usize).ok();
            canvas.clear();
            canvas.copy(&tex, None, None).ok();
        }
        canvas.present();
        thread::sleep(Duration::from_millis(10));
    }
}

#[no_mangle]
pub extern "C" fn gui_stop(_: *mut c_void) {}
#[no_mangle]
pub extern "C" fn gui_stream_delta(_: *mut c_void, _: *const c_char) {}
#[no_mangle]
pub extern "C" fn gui_set_status(_: *mut c_void, _: *const c_char) {}
#[no_mangle]
pub extern "C" fn gui_free_string(s: *mut c_char) { if !s.is_null() { unsafe { drop(CString::from_raw(s)) }; } }
