//! GDI+ draws into a premultiplied-alpha DIB; the complete ticker has alpha 1 so
//! its blank area participates in Windows hit testing.
#![allow(unsafe_op_in_unsafe_fn)]
use std::ptr::{null, null_mut};
use windows_sys::Win32::{
    Foundation::{HWND, POINT, SIZE},
    Graphics::{Gdi::*, GdiPlus::*},
    UI::WindowsAndMessaging::{ULW_ALPHA, UpdateLayeredWindow},
};

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}
pub struct Gdi {
    token: usize,
}
impl Gdi {
    pub fn new() -> anyhow::Result<Self> {
        unsafe {
            let mut input: GdiplusStartupInput = std::mem::zeroed();
            input.GdiplusVersion = 1;
            let mut token = 0;
            anyhow::ensure!(
                GdiplusStartup(&mut token, &input, null_mut()) == 0,
                "无法初始化绘图"
            );
            std::result::Result::Ok(Self { token })
        }
    }
}
impl Drop for Gdi {
    fn drop(&mut self) {
        unsafe {
            GdiplusShutdown(self.token);
        }
    }
}
pub struct Painter {
    screen: HDC,
    dc: HDC,
    bitmap: HBITMAP,
    old: HGDIOBJ,
    image: *mut GpBitmap,
    graphics: *mut GpGraphics,
    width: i32,
    height: i32,
    pub scale: f32,
}
impl Painter {
    pub fn new(width: i32, height: i32, scale: f32, background: u32) -> Option<Self> {
        if width <= 0 || height <= 0 {
            return None;
        }
        unsafe {
            let screen = GetDC(null_mut());
            let dc = CreateCompatibleDC(screen);
            let mut info: BITMAPINFO = std::mem::zeroed();
            info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            let mut pixels = null_mut();
            let bitmap =
                CreateDIBSection(screen, &info, DIB_RGB_COLORS, &mut pixels, null_mut(), 0);
            if bitmap.is_null() || dc.is_null() {
                if !bitmap.is_null() {
                    DeleteObject(bitmap);
                }
                if !dc.is_null() {
                    DeleteDC(dc);
                }
                ReleaseDC(null_mut(), screen);
                return None;
            }
            let old = SelectObject(dc, bitmap);
            let mut result = Self {
                screen,
                dc,
                bitmap,
                old,
                image: null_mut(),
                graphics: null_mut(),
                width,
                height,
                scale,
            };
            if GdipCreateBitmapFromScan0(
                width,
                height,
                width * 4,
                0x000e200b,
                pixels as *const u8,
                &mut result.image,
            ) != 0
            {
                return None;
            }
            if GdipGetImageGraphicsContext(result.image as *mut GpImage, &mut result.graphics) != 0
            {
                return None;
            }
            GdipGraphicsClear(result.graphics, background);
            GdipSetSmoothingMode(result.graphics, SmoothingModeAntiAlias);
            GdipSetTextRenderingHint(result.graphics, TextRenderingHintAntiAliasGridFit);
            Some(result)
        }
    }
    pub fn px(&self, n: f32) -> f32 {
        n * self.scale
    }
    pub fn text(&self, text: &str, rect: [f32; 4], size: f32, color: u32, bold: bool, right: bool) {
        if rect[2] <= 0. || rect[3] <= 0. {
            return;
        }
        unsafe {
            let resources = TextResources::new(size * self.scale, bold, right);
            let text = wide(text);
            let r = RectF {
                X: rect[0],
                Y: rect[1],
                Width: rect[2],
                Height: rect[3],
            };
            let mut brush = null_mut();
            GdipCreateSolidFill(color, &mut brush);
            GdipDrawString(
                self.graphics,
                text.as_ptr(),
                (text.len() - 1) as i32,
                resources.font,
                &r,
                resources.format,
                brush as *mut GpBrush,
            );
            if !brush.is_null() {
                GdipDeleteBrush(brush as *mut GpBrush);
            }
        }
    }
    pub fn measure(&self, text: &str, size: f32, bold: bool) -> f32 {
        unsafe {
            let resources = TextResources::new(size * self.scale, bold, false);
            let text = wide(text);
            let layout = RectF {
                X: 0.,
                Y: 0.,
                Width: 10000.,
                Height: 10000.,
            };
            let mut bounds: RectF = std::mem::zeroed();
            GdipMeasureString(
                self.graphics,
                text.as_ptr(),
                (text.len() - 1) as i32,
                resources.font,
                &layout,
                resources.format,
                &mut bounds,
                null_mut(),
                null_mut(),
            );
            bounds.Width.ceil()
        }
    }
    pub fn rounded(&self, rect: [f32; 4], radius: f32, color: u32) {
        unsafe {
            let mut path = null_mut();
            let mut brush = null_mut();
            GdipCreatePath(FillModeAlternate, &mut path);
            GdipCreateSolidFill(color, &mut brush);
            let [x, y, w, h] = rect;
            let d = radius * 2.;
            GdipAddPathArc(path, x, y, d, d, 180., 90.);
            GdipAddPathArc(path, x + w - d, y, d, d, 270., 90.);
            GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0., 90.);
            GdipAddPathArc(path, x, y + h - d, d, d, 90., 90.);
            GdipClosePathFigure(path);
            GdipFillPath(self.graphics, brush as *mut GpBrush, path);
            GdipDeletePath(path);
            GdipDeleteBrush(brush as *mut GpBrush);
        }
    }
    pub fn present(&self, hwnd: HWND) {
        unsafe {
            GdipFlush(self.graphics, FlushIntentionSync);
            let size = SIZE {
                cx: self.width,
                cy: self.height,
            };
            let origin = POINT { x: 0, y: 0 };
            let blend = BLENDFUNCTION {
                BlendOp: AC_SRC_OVER as u8,
                BlendFlags: 0,
                SourceConstantAlpha: 255,
                AlphaFormat: AC_SRC_ALPHA as u8,
            };
            UpdateLayeredWindow(
                hwnd,
                self.screen,
                null(),
                &size,
                self.dc,
                &origin,
                0,
                &blend,
                ULW_ALPHA,
            );
        }
    }
}
impl Drop for Painter {
    fn drop(&mut self) {
        unsafe {
            if !self.graphics.is_null() {
                GdipDeleteGraphics(self.graphics);
            }
            if !self.image.is_null() {
                GdipDisposeImage(self.image as *mut GpImage);
            }
            SelectObject(self.dc, self.old);
            DeleteObject(self.bitmap);
            DeleteDC(self.dc);
            ReleaseDC(null_mut(), self.screen);
        }
    }
}
struct TextResources {
    family: *mut GpFontFamily,
    font: *mut GpFont,
    format: *mut GpStringFormat,
}
impl TextResources {
    unsafe fn new(size: f32, bold: bool, right: bool) -> Self {
        let mut result = Self {
            family: null_mut(),
            font: null_mut(),
            format: null_mut(),
        };
        GdipCreateFontFamilyFromName(wide("Segoe UI").as_ptr(), null_mut(), &mut result.family);
        GdipCreateFont(
            result.family,
            size,
            if bold { 1 } else { 0 },
            UnitPixel,
            &mut result.font,
        );
        GdipCreateStringFormat(StringFormatFlagsNoWrap, 0, &mut result.format);
        GdipSetStringFormatAlign(
            result.format,
            if right {
                StringAlignmentFar
            } else {
                StringAlignmentNear
            },
        );
        GdipSetStringFormatLineAlign(result.format, StringAlignmentCenter);
        GdipSetStringFormatTrimming(result.format, StringTrimmingEllipsisCharacter);
        result
    }
}
impl Drop for TextResources {
    fn drop(&mut self) {
        unsafe {
            if !self.font.is_null() {
                GdipDeleteFont(self.font);
            }
            if !self.family.is_null() {
                GdipDeleteFontFamily(self.family);
            }
            if !self.format.is_null() {
                GdipDeleteStringFormat(self.format);
            }
        }
    }
}
