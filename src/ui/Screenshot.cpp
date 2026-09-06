#include "Screenshot.h"
#include <stdexcept>
#include <wincodec.h>
namespace gdv
{
namespace
{
template <class T>
struct Com
{
  T *p{};
  ~Com()
  {
    if (p)
      p->Release();
  }
  T *operator->() const { return p; }
};
void require(HRESULT hr)
{
  if (FAILED(hr))
    throw std::runtime_error("PNG capture failed (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
} // namespace
void saveScreenshot(HWND window, const std::wstring &path)
{
  if (IsIconic(window))
    throw std::runtime_error("Restore the window before capturing a screenshot.");
  RECT rect{};
  GetClientRect(window, &rect);
  if (rect.right < 1 || rect.bottom < 1 || rect.right > 8192 || rect.bottom > 8192)
    throw std::runtime_error("Invalid screenshot dimensions.");
  struct Surface
  {
    HDC screen{}, dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    ~Surface()
    {
      if (previous)
        SelectObject(dc, previous);
      if (bitmap)
        DeleteObject(bitmap);
      if (dc)
        DeleteDC(dc);
      if (screen)
        ReleaseDC(nullptr, screen);
    }
  } surface;
  surface.screen = GetDC(nullptr);
  surface.dc = CreateCompatibleDC(surface.screen);
  surface.bitmap = CreateCompatibleBitmap(surface.screen, rect.right, rect.bottom);
  if (!surface.dc || !surface.bitmap)
    throw std::runtime_error("Cannot allocate screenshot bitmap.");
  surface.previous = SelectObject(surface.dc, surface.bitmap);
  FillRect(surface.dc, &rect, GetSysColorBrush(COLOR_BTNFACE));
  // Compose direct children in client coordinates. Printing the top-level HWND itself
  // introduces the non-client frame offset and clips the bottom of a client-sized bitmap.
  HWND child = GetWindow(window, GW_CHILD);
  if (!child)
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.dc), PRF_CLIENT | PRF_ERASEBKGND);
  if (child)
    child = GetWindow(child, GW_HWNDLAST);
  for (; child; child = GetWindow(child, GW_HWNDPREV))
  {
    if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
      continue;
    RECT bounds{};
    GetWindowRect(child, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
    int saved = SaveDC(surface.dc);
    IntersectClipRect(surface.dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SetViewportOrgEx(surface.dc, bounds.left, bounds.top, nullptr);
    SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(surface.dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    // Native dropdown-list combos omit their selected text from WM_PRINT when the
    // top-level window is hidden. Render that field from the actual control state.
    wchar_t className[32]{};
    GetClassNameW(child, className, 32);
    if (std::wstring(className) == L"ComboBox")
    {
      auto selected = SendMessageW(child, CB_GETCURSEL, 0, 0);
      auto length = selected == CB_ERR ? CB_ERR : SendMessageW(child, CB_GETLBTEXTLEN, selected, 0);
      if (length != CB_ERR)
      {
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        SendMessageW(child, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(value.data()));
        COMBOBOXINFO info{sizeof(info)};
        GetComboBoxInfo(child, &info);
        auto oldFont = SelectObject(surface.dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)));
        SetBkMode(surface.dc, TRANSPARENT);
        SetTextColor(surface.dc, GetSysColor(COLOR_WINDOWTEXT));
        if (GetWindowLongPtrW(child, GWL_STYLE) & CBS_OWNERDRAWFIXED)
        {
          DRAWITEMSTRUCT item{};
          item.CtlType = ODT_COMBOBOX;
          item.CtlID = GetDlgCtrlID(child);
          item.itemID = static_cast<UINT>(selected);
          item.itemAction = ODA_DRAWENTIRE;
          item.itemState = ODS_COMBOBOXEDIT;
          item.hwndItem = child;
          item.hDC = surface.dc;
          item.rcItem = info.rcItem;
          SendMessageW(window, WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
        }
        else
          DrawTextW(surface.dc, value.c_str(), static_cast<int>(length), &info.rcItem,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(surface.dc, oldFont);
      }
    }
    RestoreDC(surface.dc, saved);
  }
  Com<IWICImagingFactory> factory;
  require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory.p)));
  Com<IWICBitmap> bitmap;
  require(factory->CreateBitmapFromHBITMAP(surface.bitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap.p));
  Com<IWICStream> stream;
  require(factory->CreateStream(&stream.p));
  require(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  Com<IWICBitmapEncoder> encoder;
  require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder.p));
  require(encoder->Initialize(stream.p, WICBitmapEncoderNoCache));
  Com<IWICBitmapFrameEncode> frame;
  require(encoder->CreateNewFrame(&frame.p, nullptr));
  require(frame->Initialize(nullptr));
  require(frame->SetSize(rect.right, rect.bottom));
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  require(frame->SetPixelFormat(&format));
  require(frame->WriteSource(bitmap.p, nullptr));
  require(frame->Commit());
  require(encoder->Commit());
}
} // namespace gdv
