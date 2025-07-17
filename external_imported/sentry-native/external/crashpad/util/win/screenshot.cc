// Copyright 2015 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/win/screenshot.h"

#include "base/logging.h"
#include "util/win/get_function.h"

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

enum GpStatus { GpOk = 0 };
struct GdiplusStartupInput {
  UINT32 GdiplusVersion;
  PVOID DebugEventCallback;
  BOOL SuppressBackgroundThread;
  BOOL SuppressExternalCodecs;
};
using GpBitmap = void;
using GpImage = void;

extern "C" {
GpStatus WINAPI GdiplusStartup(ULONG_PTR* token,
                               const GdiplusStartupInput* input,
                               void* output);
GpStatus WINAPI GdipCreateBitmapFromHBITMAP(HBITMAP hbm,
                                            HPALETTE hpal,
                                            GpBitmap** bitmap);
GpStatus WINAPI GdipSaveImageToFile(GpImage* image,
                                    const wchar_t* filename,
                                    const CLSID* encoder,
                                    const void* params);
GpStatus WINAPI GdipDisposeImage(GpImage* image);
HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd,
                                     DWORD dwAttribute,
                                     PVOID pvAttribute,
                                     DWORD cbAttribute);
}

namespace crashpad {

namespace {
bool SaveBitmap(HBITMAP bitmap, const wchar_t* path) {
  const auto gdiplus_startup = GET_FUNCTION(L"gdiplus.dll", ::GdiplusStartup);
  if (!gdiplus_startup) {
    PLOG(WARNING) << "GdiplusStartup";
    return false;
  }

  const auto gdip_create_bitmap_from_hbitmap =
      GET_FUNCTION(L"gdiplus.dll", ::GdipCreateBitmapFromHBITMAP);
  if (!gdip_create_bitmap_from_hbitmap) {
    PLOG(WARNING) << "GdipCreateBitmapFromHBITMAP";
    return false;
  }

  const auto gdip_save_image_to_file =
      GET_FUNCTION(L"gdiplus.dll", ::GdipSaveImageToFile);
  if (!gdip_save_image_to_file) {
    PLOG(WARNING) << "GdipSaveImageToFile";
    return false;
  }

  const auto gdip_dispose_image =
      GET_FUNCTION(L"gdiplus.dll", ::GdipDisposeImage);
  if (!gdip_dispose_image) {
    PLOG(WARNING) << "GdipDisposeImage";
    return false;
  }

  GdiplusStartupInput input;
  ZeroMemory(&input, sizeof(input));
  input.GdiplusVersion = 1;

  ULONG_PTR token = 0;
  if (gdiplus_startup(&token, &input, nullptr) != GpOk) {
    PLOG(WARNING) << "GdiplusStartup";
    return false;
  }

  GpImage* image = NULL;
  if (gdip_create_bitmap_from_hbitmap(bitmap, nullptr, &image) != GpOk) {
    PLOG(WARNING) << "GdipCreateBitmapFromHBITMAP";
    return false;
  }

  // CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}")
  const CLSID GdiPngEncoder = {
      0x557cf406,
      0x1a04,
      0x11d3,
      {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
  GpStatus rv = gdip_save_image_to_file(image, path, &GdiPngEncoder, nullptr);
  if (rv != GpOk) {
    PLOG(WARNING) << "GdipSaveImageToFile";
  }

  if (gdip_dispose_image(image) != GpOk) {
    PLOG(WARNING) << "GdipDisposeImage";
  }

  return rv == GpOk;
}

void CalculateRegion(DWORD pid, HRGN region) {
  const auto dwm_get_window_attribute =
      GET_FUNCTION(L"dwmapi.dll", ::DwmGetWindowAttribute);
  if (!dwm_get_window_attribute) {
    PLOG(WARNING) << "DwmGetWindowAttribute";
    return;
  }

  HWND hwnd = GetShellWindow();
  while (hwnd) {
    if (IsWindowVisible(hwnd)) {
      RECT frame = {0};
      dwm_get_window_attribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(RECT));
      if (frame.right > frame.left && frame.bottom > frame.top) {
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        HRGN wregion = CreateRectRgnIndirect(&frame);
        if (wpid == pid) {
          CombineRgn(region, region, wregion, RGN_OR);
        } else {
          CombineRgn(region, region, wregion, RGN_DIFF);
        }
        DeleteObject(wregion);
      }
    }
    hwnd = GetWindow(hwnd, GW_HWNDPREV);
  }
}
}  // namespace

bool CaptureScreenshot(ProcessID process_id, const base::FilePath& path) {
  HRGN region = CreateRectRgn(0, 0, 0, 0);
  CalculateRegion(process_id, region);

  RECT box;
  GetRgnBox(region, &box);
  LONG width = box.right - box.left;
  LONG height = box.bottom - box.top;
  if (width <= 0 || height <= 0) {
    DeleteObject(region);
    return false;
  }

  HDC src = GetDC(NULL);
  HDC hdc = CreateCompatibleDC(src);
  HBITMAP bitmap = CreateCompatibleBitmap(src, width, height);
  SelectObject(hdc, bitmap);
  OffsetRgn(region, -box.left, -box.top);
  SelectClipRgn(hdc, region);
  BitBlt(hdc, 0, 0, width, height, src, box.left, box.top, SRCCOPY);

  bool rv = SaveBitmap(bitmap, path.value().c_str());
  if (!rv) {
    LOG(WARNING) << "Failed to save screenshot: " << path;
  }

  DeleteObject(bitmap);
  DeleteDC(hdc);
  ReleaseDC(NULL, src);
  DeleteObject(region);
  return rv;
}

}  // namespace crashpad
