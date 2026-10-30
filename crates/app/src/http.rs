use anyhow::{Context, Result, ensure};
use crypto_taskbar_core::{Source, expand_header, validate_url};
use serde_json::Value;
use std::{
    ffi::c_void,
    ptr::{null, null_mut},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};
use windows_sys::Win32::{Foundation::GetLastError, Networking::WinHttp::*};

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}
struct Internet(*mut c_void);
impl Drop for Internet {
    fn drop(&mut self) {
        unsafe {
            WinHttpCloseHandle(self.0);
        }
    }
}
impl Internet {
    fn checked(raw: *mut c_void) -> Result<Self> {
        ensure!(!raw.is_null(), "无法创建网络请求（{}）", unsafe {
            GetLastError()
        });
        Ok(Self(raw))
    }
}
pub struct Client {
    session: Internet,
}
impl Client {
    pub fn new() -> Result<Self> {
        let name = wide(&format!("CryptoTaskbar/{}", crypto_taskbar_core::VERSION));
        let session = Internet::checked(unsafe {
            WinHttpOpen(
                name.as_ptr(),
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                null(),
                null(),
                0,
            )
        })?;
        unsafe {
            WinHttpSetTimeouts(session.0, 2000, 3000, 3000, 4000);
        }
        Ok(Self { session })
    }
    pub fn get(&self, url: &str, source: &Source, stop: &AtomicBool) -> Result<Value> {
        validate_url(url)?;
        ensure!(!stop.load(Ordering::Relaxed), "请求已取消");
        let url = wide(url);
        unsafe {
            let mut parts: URL_COMPONENTS = std::mem::zeroed();
            parts.dwStructSize = std::mem::size_of::<URL_COMPONENTS>() as u32;
            parts.dwHostNameLength = u32::MAX;
            parts.dwUrlPathLength = u32::MAX;
            parts.dwExtraInfoLength = u32::MAX;
            ensure!(
                WinHttpCrackUrl(url.as_ptr(), 0, 0, &mut parts) != 0,
                "接口 URL 无效。"
            );
            let host = String::from_utf16_lossy(std::slice::from_raw_parts(
                parts.lpszHostName,
                parts.dwHostNameLength as usize,
            ));
            let mut path = if parts.dwUrlPathLength > 0 {
                String::from_utf16_lossy(std::slice::from_raw_parts(
                    parts.lpszUrlPath,
                    parts.dwUrlPathLength as usize,
                ))
            } else {
                "/".into()
            };
            if parts.dwExtraInfoLength > 0 {
                path.push_str(&String::from_utf16_lossy(std::slice::from_raw_parts(
                    parts.lpszExtraInfo,
                    parts.dwExtraInfoLength as usize,
                )));
            }
            let connection = Internet::checked(WinHttpConnect(
                self.session.0,
                wide(&host).as_ptr(),
                parts.nPort,
                0,
            ))?;
            let request = Internet::checked(WinHttpOpenRequest(
                connection.0,
                wide("GET").as_ptr(),
                wide(&path).as_ptr(),
                null(),
                null(),
                null(),
                if parts.nScheme == 2 {
                    WINHTTP_FLAG_SECURE
                } else {
                    0
                },
            ))?;
            let redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(
                request.0,
                WINHTTP_OPTION_REDIRECT_POLICY,
                &redirect as *const _ as *const c_void,
                4,
            );
            let compression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
            WinHttpSetOption(
                request.0,
                WINHTTP_OPTION_DECOMPRESSION,
                &compression as *const _ as *const c_void,
                4,
            );
            let mut headers = "Accept: application/json\r\n".to_owned();
            for (name, value) in &source.headers {
                headers.push_str(&format!("{name}: {}\r\n", expand_header(value)?));
            }
            let headers = wide(&headers);
            ensure!(
                WinHttpSendRequest(
                    request.0,
                    headers.as_ptr(),
                    (headers.len() - 1) as u32,
                    null(),
                    0,
                    0,
                    0
                ) != 0
                    && WinHttpReceiveResponse(request.0, null_mut()) != 0,
                "网络错误 {}",
                GetLastError()
            );
            let mut status = 0u32;
            let mut size = 4;
            ensure!(
                WinHttpQueryHeaders(
                    request.0,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    null(),
                    &mut status as *mut _ as *mut c_void,
                    &mut size,
                    null_mut()
                ) != 0,
                "无法读取 HTTP 状态。"
            );
            ensure!(status == 200, "HTTP {status}");
            let start = Instant::now();
            let mut body = Vec::new();
            let mut buffer = [0u8; 8192];
            loop {
                ensure!(!stop.load(Ordering::Relaxed), "请求已取消");
                let mut read = 0;
                ensure!(
                    WinHttpReadData(
                        request.0,
                        buffer.as_mut_ptr() as *mut c_void,
                        buffer.len() as u32,
                        &mut read
                    ) != 0,
                    "读取响应失败（{}）",
                    GetLastError()
                );
                if read == 0 {
                    break;
                }
                body.extend_from_slice(&buffer[..read as usize]);
                ensure!(body.len() <= 2 * 1024 * 1024, "响应超过 2 MiB。");
                ensure!(start.elapsed() < Duration::from_secs(8), "响应超时。");
            }
            serde_json::from_slice(&body).context("接口返回的 JSON 无效")
        }
    }
}
