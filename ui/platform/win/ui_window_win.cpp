// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/platform/win/ui_window_win.h"

#include "ui/inactive_press.h"
#include "ui/platform/win/ui_window_title_win.h"
#include "base/platform/base_platform_info.h"
#include "styles/palette.h"

#include <QtCore/QAbstractNativeEventFilter>
#include <QtGui/QWindow>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QApplication>
#include <qpa/qplatformnativeinterface.h>

#include <dwmapi.h>
#include <uxtheme.h>

Q_DECLARE_METATYPE(QMargins);

namespace Ui {
namespace Platform {
namespace {

bool IsCompositionEnabled() {
	auto result = BOOL(FALSE);
	const auto success = (DwmIsCompositionEnabled(&result) == S_OK);
	return success && result;
}

} // namespace

class WindowHelper::NativeFilter final : public QAbstractNativeEventFilter {
public:
	void registerWindow(HWND handle, not_null<WindowHelper*> helper);
	void unregisterWindow(HWND handle);

	bool nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		long *result) override;

private:
	base::flat_map<HWND, not_null<WindowHelper*>> _windowByHandle;

};

void WindowHelper::NativeFilter::registerWindow(
		HWND handle,
		not_null<WindowHelper*> helper) {
	_windowByHandle.emplace(handle, helper);
}

void WindowHelper::NativeFilter::unregisterWindow(HWND handle) {
	_windowByHandle.remove(handle);
}

bool WindowHelper::NativeFilter::nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		long *result) {
	const auto msg = static_cast<MSG*>(message);
	const auto i = _windowByHandle.find(msg->hwnd);
	return (i != end(_windowByHandle))
		? i->second->handleNativeEvent(
			msg->message,
			msg->wParam,
			msg->lParam,
			static_cast<LRESULT*>(result))
		: false;
}

WindowHelper::WindowHelper(not_null<RpWidget*> window)
: _window(window)
, _handle(GetWindowHandle(_window))
, _title(Ui::CreateChild<TitleWidget>(_window.get()))
, _body(Ui::CreateChild<RpWidget>(_window.get()))
, _shadow(_window, st::windowShadowFg->c) {
	Expects(_handle != nullptr);

	GetNativeFilter()->registerWindow(_handle, this);
	init();
}

WindowHelper::~WindowHelper() {
	GetNativeFilter()->unregisterWindow(_handle);
}

not_null<RpWidget*> WindowHelper::body() {
	return _body;
}

void WindowHelper::setTitle(const QString &title) {
	_title->setText(title);
	_window->setWindowTitle(title);
}

void WindowHelper::setSizeMin(QSize size) {
	_window->setMinimumSize(size.width(), _title->height() + size.height());
}

void WindowHelper::init() {
	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		_shadow.setColor(st::windowShadowFg->c);
	}, _window->lifetime());

	rpl::combine(
		_window->sizeValue(),
		_title->heightValue()
	) | rpl::start_with_next([=](QSize size, int titleHeight) {
		_body->setGeometry(
			0,
			titleHeight,
			size.width(),
			size.height() - titleHeight);
	}, _body->lifetime());

	updateMargins();

	if (!::Platform::IsWindows8OrGreater()) {
		SetWindowTheme(_handle, L" ", L" ");
		QApplication::setStyle(QStyleFactory::create("Windows"));
	}

	_menu = GetSystemMenu(_handle, FALSE);
	updateSystemMenu();
	Ui::Connect(
		_window->windowHandle(),
		&QWindow::windowStateChanged,
		[=](Qt::WindowState state) { updateSystemMenu(state); });
}

bool WindowHelper::handleNativeEvent(
		UINT msg,
		WPARAM wParam,
		LPARAM lParam,
		LRESULT *result) {
	switch (msg) {

	case WM_ACTIVATE: {
		if (LOWORD(wParam) == WA_CLICKACTIVE) {
			Ui::MarkInactivePress(_window, true);
		}
		if (LOWORD(wParam) != WA_INACTIVE) {
			_shadow.update(WindowShadow::Change::Activate);
		} else {
			_shadow.update(WindowShadow::Change::Deactivate);
		}
		_window->update();
	} return false;

	case WM_NCPAINT: {
		if (::Platform::IsWindows8OrGreater()) {
			return false;
		}
		if (result) *result = 0;
	} return true;

	case WM_NCCALCSIZE: {
		WINDOWPLACEMENT wp;
		wp.length = sizeof(WINDOWPLACEMENT);
		if (GetWindowPlacement(_handle, &wp)
			&& (wp.showCmd == SW_SHOWMAXIMIZED)) {
			const auto params = (LPNCCALCSIZE_PARAMS)lParam;
			const auto r = (wParam == TRUE)
				? &params->rgrc[0]
				: (LPRECT)lParam;
			const auto hMonitor = MonitorFromPoint(
				{ (r->left + r->right) / 2, (r->top + r->bottom) / 2 },
				MONITOR_DEFAULTTONEAREST);
			if (hMonitor) {
				MONITORINFO mi;
				mi.cbSize = sizeof(mi);
				if (GetMonitorInfo(hMonitor, &mi)) {
					*r = mi.rcWork;
				}
			}
		}
		if (result) *result = 0;
		return true;
	}

	case WM_NCACTIVATE: {
		if (IsCompositionEnabled()) {
			const auto res = DefWindowProc(_handle, msg, wParam, -1);
			if (result) *result = res;
		} else {
			// Thanks https://github.com/melak47/BorderlessWindow
			if (result) *result = 1;
		}
	} return true;

	case WM_WINDOWPOSCHANGING:
	case WM_WINDOWPOSCHANGED: {
		WINDOWPLACEMENT wp;
		wp.length = sizeof(WINDOWPLACEMENT);
		if (GetWindowPlacement(_handle, &wp)
			&& (wp.showCmd == SW_SHOWMAXIMIZED
				|| wp.showCmd == SW_SHOWMINIMIZED)) {
			_shadow.update(WindowShadow::Change::Hidden);
		} else {
			_shadow.update(
				WindowShadow::Change::Moved | WindowShadow::Change::Resized,
				(WINDOWPOS*)lParam);
		}
	} return false;

	case WM_SIZE: {
		if (wParam == SIZE_MAXIMIZED
			|| wParam == SIZE_RESTORED
			|| wParam == SIZE_MINIMIZED) {
			if (wParam != SIZE_RESTORED
				|| _window->windowState() != Qt::WindowNoState) {
				Qt::WindowState state = Qt::WindowNoState;
				if (wParam == SIZE_MAXIMIZED) {
					state = Qt::WindowMaximized;
				} else if (wParam == SIZE_MINIMIZED) {
					state = Qt::WindowMinimized;
				}
				emit _window->windowHandle()->windowStateChanged(state);
			}
			updateMargins();
			const auto changes = (wParam == SIZE_MINIMIZED
				|| wParam == SIZE_MAXIMIZED)
				? WindowShadow::Change::Hidden
				: (WindowShadow::Change::Resized
					| WindowShadow::Change::Shown);
			_shadow.update(changes);
		}
	} return false;

	case WM_SHOWWINDOW: {
		const auto style = GetWindowLong(_handle, GWL_STYLE);
		const auto changes = WindowShadow::Change::Resized
			| ((wParam && !(style & (WS_MAXIMIZE | WS_MINIMIZE)))
				? WindowShadow::Change::Shown
				: WindowShadow::Change::Hidden);
		_shadow.update(changes);
	} return false;

	case WM_MOVE: {
		_shadow.update(WindowShadow::Change::Moved);
	} return false;

	case WM_NCHITTEST: {
		if (!result) {
			return false;
		}

		const auto p = MAKEPOINTS(lParam);
		auto r = RECT();
		GetWindowRect(_handle, &r);
		const auto mapped = QPoint(
			p.x - r.left + _marginsDelta.left(),
			p.y - r.top + _marginsDelta.top());
		if (!_window->rect().contains(mapped)) {
			*result = HTTRANSPARENT;
		} else if (!_title->geometry().contains(mapped)) {
			*result = HTCLIENT;
		} else switch (_title->hitTest(_title->pos() + mapped)) {
		case HitTestResult::Client:
		case HitTestResult::SysButton:   *result = HTCLIENT; break;
		case HitTestResult::Caption:     *result = HTCAPTION; break;
		case HitTestResult::Top:         *result = HTTOP; break;
		case HitTestResult::TopRight:    *result = HTTOPRIGHT; break;
		case HitTestResult::Right:       *result = HTRIGHT; break;
		case HitTestResult::BottomRight: *result = HTBOTTOMRIGHT; break;
		case HitTestResult::Bottom:      *result = HTBOTTOM; break;
		case HitTestResult::BottomLeft:  *result = HTBOTTOMLEFT; break;
		case HitTestResult::Left:        *result = HTLEFT; break;
		case HitTestResult::TopLeft:     *result = HTTOPLEFT; break;
		case HitTestResult::None:
		default:                         *result = HTTRANSPARENT; break;
		};
	} return true;

	case WM_NCRBUTTONUP: {
		SendMessage(_handle, WM_SYSCOMMAND, SC_MOUSEMENU, lParam);
	} return true;

	case WM_SYSCOMMAND: {
		if (wParam == SC_MOUSEMENU) {
			POINTS p = MAKEPOINTS(lParam);
			updateSystemMenu(_window->windowHandle()->windowState());
			TrackPopupMenu(
				_menu,
				TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
				p.x,
				p.y,
				0,
				_handle,
				0);
		}
	} return false;

	case WM_COMMAND: {
		if (HIWORD(wParam)) {
			return false;
		}
		const auto command = LOWORD(wParam);
		switch (command) {
		case SC_CLOSE: _window->close(); return true;
		case SC_MINIMIZE:
			_window->setWindowState(Qt::WindowMinimized);
			return true;
		case SC_MAXIMIZE:
			_window->setWindowState(Qt::WindowMaximized);
			return true;
		case SC_RESTORE:
			_window->setWindowState(Qt::WindowNoState);
			return true;
		}
	} return true;

	}
	return false;
}

void WindowHelper::updateMargins() {
	if (_updatingMargins) return;

	_updatingMargins = true;
	const auto guard = gsl::finally([&] { _updatingMargins = false; });

	RECT r, a;

	GetClientRect(_handle, &r);
	a = r;

	const auto style = GetWindowLong(_handle, GWL_STYLE);
	const auto styleEx = GetWindowLong(_handle, GWL_EXSTYLE);
	AdjustWindowRectEx(&a, style, false, styleEx);
	auto margins = QMargins(
		a.left - r.left,
		a.top - r.top,
		r.right - a.right,
		r.bottom - a.bottom);
	if (style & WS_MAXIMIZE) {
		RECT w, m;
		GetWindowRect(_handle , &w);
		m = w;

		HMONITOR hMonitor = MonitorFromRect(&w, MONITOR_DEFAULTTONEAREST);
		if (hMonitor) {
			MONITORINFO mi;
			mi.cbSize = sizeof(mi);
			GetMonitorInfo(hMonitor, &mi);
			m = mi.rcWork;
		}

		_marginsDelta = QMargins(
			w.left - m.left,
			w.top - m.top,
			m.right - w.right,
			m.bottom - w.bottom);

		margins.setLeft(margins.left() - _marginsDelta.left());
		margins.setRight(margins.right() - _marginsDelta.right());
		margins.setBottom(margins.bottom() - _marginsDelta.bottom());
		margins.setTop(margins.top() - _marginsDelta.top());
	} else if (!_marginsDelta.isNull()) {
		RECT w;
		GetWindowRect(_handle, &w);
		SetWindowPos(
			_handle,
			0,
			0,
			0,
			w.right - w.left - _marginsDelta.left() - _marginsDelta.right(),
			w.bottom - w.top - _marginsDelta.top() - _marginsDelta.bottom(),
			(SWP_NOMOVE
				| SWP_NOSENDCHANGING
				| SWP_NOZORDER
				| SWP_NOACTIVATE
				| SWP_NOREPOSITION));
		_marginsDelta = QMargins();
	}

	if (const auto native = QGuiApplication::platformNativeInterface()) {
		native->setWindowProperty(
			_window->windowHandle()->handle(),
			"WindowsCustomMargins",
			QVariant::fromValue<QMargins>(margins));
	}
}

void WindowHelper::updateSystemMenu() {
	updateSystemMenu(_window->windowHandle()->windowState());
}

void WindowHelper::updateSystemMenu(Qt::WindowState state) {
	if (!_menu) {
		return;
	}

	const auto menuToDisable = (state == Qt::WindowMaximized)
		? SC_MAXIMIZE
		: (state == Qt::WindowMinimized)
		? SC_MINIMIZE
		: SC_RESTORE;
	const auto itemCount = GetMenuItemCount(_menu);
	for (int i = 0; i < itemCount; ++i) {
		MENUITEMINFO itemInfo = { 0 };
		itemInfo.cbSize = sizeof(itemInfo);
		itemInfo.fMask = MIIM_TYPE | MIIM_STATE | MIIM_ID;
		if (!GetMenuItemInfo(_menu, i, TRUE, &itemInfo)) {
			break;
		}
		if (itemInfo.fType & MFT_SEPARATOR) {
			continue;
		} else if (!itemInfo.wID || itemInfo.wID == SC_CLOSE) {
			continue;
		}
		UINT fOldState = itemInfo.fState;
		UINT fState = itemInfo.fState & ~(MFS_DISABLED | MFS_DEFAULT);
		if (itemInfo.wID == menuToDisable
			|| (itemInfo.wID != SC_MINIMIZE
				&& itemInfo.wID != SC_MAXIMIZE
				&& itemInfo.wID != SC_RESTORE)) {
			fState |= MFS_DISABLED;
		}
		itemInfo.fMask = MIIM_STATE;
		itemInfo.fState = fState;
		if (!SetMenuItemInfo(_menu, i, TRUE, &itemInfo)) {
			break;
		}
	}
}

not_null<WindowHelper::NativeFilter*> WindowHelper::GetNativeFilter() {
	Expects(QCoreApplication::instance() != nullptr);

	static const auto GlobalFilter = [&] {
		const auto application = QCoreApplication::instance();
		const auto filter = Ui::CreateChild<NativeFilter>(application);
		application->installNativeEventFilter(filter);
		return filter;
	}();
	return GlobalFilter;
}

HWND GetWindowHandle(not_null<RpWidget*> widget) {
	widget->window()->createWinId();

	const auto window = widget->window()->windowHandle();
	const auto native = QGuiApplication::platformNativeInterface();
	Assert(window != nullptr);
	Assert(native != nullptr);

	return static_cast<HWND>(native->nativeResourceForWindow(
		QByteArrayLiteral("handle"),
		window));
}

std::unique_ptr<BasicWindowHelper> CreateWindowHelper(
	not_null<RpWidget*> window) {
	return std::make_unique<WindowHelper>(window);
}

} // namespace Platform
} // namespace Ui