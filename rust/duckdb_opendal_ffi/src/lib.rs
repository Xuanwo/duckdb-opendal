use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use std::sync::OnceLock;

use bytes::Buf;
use opendal::options::ReadOptions;
use opendal::{EntryMode, ErrorKind, Operator};
use tokio::runtime::Runtime;

static RUNTIME: OnceLock<Runtime> = OnceLock::new();

fn runtime() -> &'static Runtime {
    RUNTIME.get_or_init(|| Runtime::new().expect("tokio runtime must be created"))
}

fn set_error(out_error: *mut *mut c_char, err: impl ToString) {
    if out_error.is_null() {
        return;
    }
    let s = CString::new(err.to_string()).unwrap_or_else(|_| CString::new("error").unwrap());
    unsafe {
        *out_error = s.into_raw();
    }
}

#[repr(C)]
pub struct duckdb_opendal_operator {
    op: Operator,
}

#[repr(C)]
pub struct duckdb_opendal_lister {
    entries: Vec<opendal::Entry>,
    idx: usize,
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_string_free(s: *mut c_char) {
    if s.is_null() {
        return;
    }
    unsafe {
        drop(CString::from_raw(s));
    }
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_operator_new(
    service: *const c_char,
    keys: *const *const c_char,
    values: *const *const c_char,
    len: usize,
    out_error: *mut *mut c_char,
) -> *mut duckdb_opendal_operator {
    if service.is_null() {
        set_error(out_error, "service is null");
        return ptr::null_mut();
    }
    let service = unsafe { CStr::from_ptr(service) };
    let service = match service.to_str() {
        Ok(v) => v,
        Err(_) => {
            set_error(out_error, "service is not valid utf-8");
            return ptr::null_mut();
        }
    };

    if len > 0 && (keys.is_null() || values.is_null()) {
        set_error(out_error, "keys/values is null");
        return ptr::null_mut();
    }

    let mut opts: Vec<(String, String)> = Vec::with_capacity(len);
    for i in 0..len {
        let k = unsafe { *keys.add(i) };
        let v = unsafe { *values.add(i) };
        if k.is_null() || v.is_null() {
            set_error(out_error, "keys/values contains null");
            return ptr::null_mut();
        }
        let k = unsafe { CStr::from_ptr(k) }
            .to_str()
            .map(|v| v.to_string())
            .map_err(|_| "key is not valid utf-8");
        let v = unsafe { CStr::from_ptr(v) }
            .to_str()
            .map(|v| v.to_string())
            .map_err(|_| "value is not valid utf-8");
        let (k, v) = match (k, v) {
            (Ok(k), Ok(v)) => (k, v),
            (Err(e), _) | (_, Err(e)) => {
                set_error(out_error, e);
                return ptr::null_mut();
            }
        };
        opts.push((k, v));
    }

    let op = match Operator::via_iter(service, opts) {
        Ok(op) => op,
        Err(e) => {
            set_error(out_error, e);
            return ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(duckdb_opendal_operator { op }))
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_operator_free(op: *mut duckdb_opendal_operator) {
    if op.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(op));
    }
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_stat(
    op: *mut duckdb_opendal_operator,
    path: *const c_char,
    out_exists: *mut bool,
    out_is_dir: *mut bool,
    out_size: *mut u64,
    out_error: *mut *mut c_char,
) -> bool {
    if op.is_null() || path.is_null() {
        set_error(out_error, "op/path is null");
        return false;
    }

    let path = unsafe { CStr::from_ptr(path) };
    let path = match path.to_str() {
        Ok(v) => v,
        Err(_) => {
            set_error(out_error, "path is not valid utf-8");
            return false;
        }
    };

    let operator = unsafe { &(*op).op };

    let res = runtime().block_on(async { operator.stat(path).await });
    match res {
        Ok(meta) => {
            unsafe {
                if !out_exists.is_null() {
                    *out_exists = true;
                }
                if !out_is_dir.is_null() {
                    *out_is_dir = meta.mode() == EntryMode::DIR;
                }
                if !out_size.is_null() {
                    *out_size = meta.content_length();
                }
            }
            true
        }
        Err(e) if e.kind() == ErrorKind::NotFound => {
            unsafe {
                if !out_exists.is_null() {
                    *out_exists = false;
                }
                if !out_is_dir.is_null() {
                    *out_is_dir = false;
                }
                if !out_size.is_null() {
                    *out_size = 0;
                }
            }
            true
        }
        Err(e) => {
            set_error(out_error, e);
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_read(
    op: *mut duckdb_opendal_operator,
    path: *const c_char,
    offset: u64,
    buffer: *mut u8,
    buffer_len: usize,
    out_bytes_read: *mut usize,
    out_error: *mut *mut c_char,
) -> bool {
    if op.is_null() || path.is_null() || (buffer.is_null() && buffer_len > 0) {
        set_error(out_error, "invalid arguments");
        return false;
    }

    let path = unsafe { CStr::from_ptr(path) };
    let path = match path.to_str() {
        Ok(v) => v,
        Err(_) => {
            set_error(out_error, "path is not valid utf-8");
            return false;
        }
    };

    let operator = unsafe { &(*op).op };
    let range_end = offset.saturating_add(buffer_len as u64);
    let opts = ReadOptions {
        range: (offset..range_end).into(),
        ..Default::default()
    };

    let res = runtime().block_on(async { operator.read_options(path, opts).await });
    let mut data = match res {
        Ok(buf) => buf,
        Err(e) => {
            set_error(out_error, e);
            return false;
        }
    };

    let mut copied: usize = 0;
    unsafe {
        let out_slice = std::slice::from_raw_parts_mut(buffer, buffer_len);
        while data.has_remaining() && copied < out_slice.len() {
            let chunk = data.chunk();
            let to_copy = (out_slice.len() - copied).min(chunk.len());
            out_slice[copied..copied + to_copy].copy_from_slice(&chunk[..to_copy]);
            copied += to_copy;
            data.advance(to_copy);
        }
        if !out_bytes_read.is_null() {
            *out_bytes_read = copied;
        }
    }

    true
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_list(
    op: *mut duckdb_opendal_operator,
    path: *const c_char,
    recursive: bool,
    out_error: *mut *mut c_char,
) -> *mut duckdb_opendal_lister {
    if op.is_null() || path.is_null() {
        set_error(out_error, "op/path is null");
        return ptr::null_mut();
    }
    let path = unsafe { CStr::from_ptr(path) };
    let path = match path.to_str() {
        Ok(v) => v,
        Err(_) => {
            set_error(out_error, "path is not valid utf-8");
            return ptr::null_mut();
        }
    };

    let operator = unsafe { &(*op).op };
    let res = runtime().block_on(async {
        operator
            .list_with(path)
            .recursive(recursive)
            .await
    });

    let entries = match res {
        Ok(v) => v,
        Err(e) if e.kind() == ErrorKind::NotFound => Vec::new(),
        Err(e) => {
            set_error(out_error, e);
            return ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(duckdb_opendal_lister { entries, idx: 0 }))
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_lister_next(
    lister: *mut duckdb_opendal_lister,
    out_path: *mut *mut c_char,
    out_is_dir: *mut bool,
    out_size: *mut u64,
) -> bool {
    if lister.is_null() {
        return false;
    }

    let l = unsafe { &mut *lister };
    if l.idx >= l.entries.len() {
        return false;
    }
    let entry = &l.entries[l.idx];
    l.idx += 1;

    let p = CString::new(entry.path()).unwrap_or_else(|_| CString::new("path").unwrap());
    unsafe {
        if !out_path.is_null() {
            *out_path = p.into_raw();
        }
        if !out_is_dir.is_null() {
            *out_is_dir = entry.metadata().mode() == EntryMode::DIR;
        }
        if !out_size.is_null() {
            *out_size = entry.metadata().content_length();
        }
    }
    true
}

#[no_mangle]
pub extern "C" fn duckdb_opendal_lister_free(lister: *mut duckdb_opendal_lister) {
    if lister.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(lister));
    }
}

