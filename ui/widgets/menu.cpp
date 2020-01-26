// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/menu.h"

#include "ui/effects/ripple_animation.h"
#include "ui/widgets/checkbox.h"
#include "ui/text/text.h"

#include <QtGui/QtEvents>

namespace Ui {
namespace {

[[nodiscard]] TextWithEntities ParseMenuItem(const QString &text) {
	auto result = TextWithEntities();
	result.text.reserve(text.size());
	auto afterAmpersand = false;
	for (const auto ch : text) {
		if (afterAmpersand) {
			afterAmpersand = false;
			if (ch == '&') {
				result.text.append(ch);
			} else {
				result.entities.append(EntityInText{
					EntityType::Underline,
					result.text.size(),
					1 });
				result.text.append(ch);
			}
		} else if (ch == '&') {
			afterAmpersand = true;
		} else {
			result.text.append(ch);
		}
	}
	return result;
}

TextParseOptions MenuTextOptions = {
	TextParseLinks | TextParseRichText, // flags
	0, // maxw
	0, // maxh
	Qt::LayoutDirectionAuto, // dir
};

} // namespace

struct Menu::ActionData {
	Text::String text;
	QString shortcut;
	const style::icon *icon = nullptr;
	const style::icon *iconOver = nullptr;
	std::unique_ptr<RippleAnimation> ripple;
	std::unique_ptr<ToggleView> toggle;
	int textWidth = 0;
	bool hasSubmenu = false;
};

Menu::Menu(QWidget *parent, const style::Menu &st)
: RpWidget(parent)
, _st(st)
, _itemHeight(_st.itemPadding.top() + _st.itemStyle.font->height + _st.itemPadding.bottom())
, _separatorHeight(_st.separatorPadding.top() + _st.separatorWidth + _st.separatorPadding.bottom()) {
	init();
}

Menu::Menu(QWidget *parent, QMenu *menu, const style::Menu &st)
: RpWidget(parent)
, _st(st)
, _wappedMenu(menu)
, _itemHeight(_st.itemPadding.top() + _st.itemStyle.font->height + _st.itemPadding.bottom())
, _separatorHeight(_st.separatorPadding.top() + _st.separatorWidth + _st.separatorPadding.bottom()) {
	init();

	_wappedMenu->setParent(this);
	for (auto action : _wappedMenu->actions()) {
		addAction(action);
	}
	_wappedMenu->hide();
}

Menu::~Menu() = default;

void Menu::init() {
	resize(_forceWidth ? _forceWidth : _st.widthMin, _st.skip * 2);

	setMouseTracking(true);

	if (_st.itemBg->c.alpha() == 255) {
		setAttribute(Qt::WA_OpaquePaintEvent);
	}
}

not_null<QAction*> Menu::addAction(const QString &text, const QObject *receiver, const char* member, const style::icon *icon, const style::icon *iconOver) {
	const auto action = addAction(new QAction(text, this), icon, iconOver);
	connect(action, SIGNAL(triggered(bool)), receiver, member, Qt::QueuedConnection);
	return action;
}

not_null<QAction*> Menu::addAction(const QString &text, Fn<void()> callback, const style::icon *icon, const style::icon *iconOver) {
	const auto action = addAction(new QAction(text, this), icon, iconOver);
	connect(action, &QAction::triggered, action, std::move(callback), Qt::QueuedConnection);
	return action;
}

not_null<QAction*> Menu::addAction(const QString &text, std::unique_ptr<QMenu> submenu) {
	const auto action = new QAction(text, this);
	action->setMenu(submenu.release());
	return addAction(action, nullptr, nullptr);
}

not_null<QAction*> Menu::addAction(not_null<QAction*> action, const style::icon *icon, const style::icon *iconOver) {
	connect(action, &QAction::changed, this, [=] {
		actionChanged();
	});
	_actions.emplace_back(action);
	_actionsData.push_back([&] {
		auto data = ActionData();
		data.icon = icon;
		data.iconOver = iconOver ? iconOver : icon;
		data.hasSubmenu = (action->menu() != nullptr);
		return data;
	}());

	auto newWidth = qMax(width(), _st.widthMin);
	newWidth = processAction(action, _actions.size() - 1, newWidth);
	auto newHeight = height() + (action->isSeparator() ? _separatorHeight : _itemHeight);
	resize(_forceWidth ? _forceWidth : newWidth, newHeight);
	if (_resizedCallback) {
		_resizedCallback();
	}
	updateSelected(QCursor::pos());
	update();

	return action;
}

not_null<QAction*> Menu::addSeparator() {
	const auto separator = new QAction(this);
	separator->setSeparator(true);
	return addAction(separator);
}

void Menu::clearActions() {
	setSelected(-1);
	setPressed(-1);
	_actionsData.clear();
	for (auto action : base::take(_actions)) {
		if (action->parent() == this) {
			delete action;
		}
	}
	resize(_forceWidth ? _forceWidth : _st.widthMin, _st.skip * 2);
	if (_resizedCallback) {
		_resizedCallback();
	}
}

void Menu::finishAnimating() {
	for (auto &data : _actionsData) {
		if (data.ripple) {
			data.ripple.reset();
		}
		if (data.toggle) {
			data.toggle->finishAnimating();
		}
	}
}

int Menu::processAction(not_null<QAction*> action, int index, int width) {
	auto &data = _actionsData[index];
	if (action->isSeparator() || action->text().isEmpty()) {
		data.shortcut = QString();
		data.text.clear();
	} else {
		auto actionTextParts = action->text().split('\t');
		auto actionText = actionTextParts.empty() ? QString() : actionTextParts[0];
		auto actionShortcut = (actionTextParts.size() > 1) ? actionTextParts[1] : QString();
		data.text.setMarkedText(_st.itemStyle, ParseMenuItem(actionText), MenuTextOptions);
		const auto textw = data.text.maxWidth();
		int goodw = _st.itemPadding.left() + textw + _st.itemPadding.right();
		if (data.hasSubmenu) {
			goodw += _st.itemPadding.right() + _st.arrow.width();
		} else if (!actionShortcut.isEmpty()) {
			goodw += _st.itemPadding.right() + _st.itemStyle.font->width(actionShortcut);
		}
		if (action->isCheckable()) {
			auto updateCallback = [this, index] { updateItem(index); };
			if (data.toggle) {
				data.toggle->setUpdateCallback(updateCallback);
				data.toggle->setChecked(action->isChecked(), anim::type::normal);
			} else {
				data.toggle = std::make_unique<ToggleView>(_st.itemToggle, action->isChecked(), updateCallback);
			}
			goodw += _st.itemPadding.right() + data.toggle->getSize().width() - _st.itemToggleShift;
		} else {
			data.toggle.reset();
		}
		width = std::clamp(goodw, width, _st.widthMax);
		data.textWidth = width - (goodw - textw);
		data.shortcut = actionShortcut;
	}
	return width;
}

void Menu::setShowSource(TriggeredSource source) {
	_mouseSelection = (source == TriggeredSource::Mouse);
	setSelected((source == TriggeredSource::Mouse || _actions.empty()) ? -1 : 0);
}

const std::vector<not_null<QAction*>> &Menu::actions() const {
	return _actions;
}

void Menu::setForceWidth(int forceWidth) {
	_forceWidth = forceWidth;
	resize(_forceWidth, height());
}

void Menu::actionChanged() {
	auto newWidth = _st.widthMin;
	auto index = 0;
	for (const auto action : _actions) {
		newWidth = processAction(action, index++, newWidth);
	}
	if (newWidth != width() && !_forceWidth) {
		resize(newWidth, height());
		if (_resizedCallback) {
			_resizedCallback();
		}
	}
	update();
}

void Menu::paintEvent(QPaintEvent *e) {
	Painter p(this);

	auto clip = e->rect();

	auto topskip = QRect(0, 0, width(), _st.skip);
	auto bottomskip = QRect(0, height() - _st.skip, width(), _st.skip);
	if (clip.intersects(topskip)) p.fillRect(clip.intersected(topskip), _st.itemBg);
	if (clip.intersects(bottomskip)) p.fillRect(clip.intersected(bottomskip), _st.itemBg);

	int top = _st.skip;
	p.translate(0, top);
	p.setFont(_st.itemStyle.font);
	for (int i = 0, count = int(_actions.size()); i != count; ++i) {
		if (clip.top() + clip.height() <= top) break;

		const auto action = _actions[i];
		auto &data = _actionsData[i];
		auto actionHeight = action->isSeparator() ? _separatorHeight : _itemHeight;
		top += actionHeight;
		if (clip.top() < top) {
			if (action->isSeparator()) {
				p.fillRect(0, 0, width(), actionHeight, _st.itemBg);
				p.fillRect(_st.separatorPadding.left(), _st.separatorPadding.top(), width() - _st.separatorPadding.left() - _st.separatorPadding.right(), _st.separatorWidth, _st.separatorFg);
			} else {
				auto enabled = action->isEnabled();
				auto selected = ((i == _selected || i == _pressed) && enabled);
				if (selected && _st.itemBgOver->c.alpha() < 255) {
					p.fillRect(0, 0, width(), actionHeight, _st.itemBg);
				}
				p.fillRect(0, 0, width(), actionHeight, selected ? _st.itemBgOver : _st.itemBg);
				if (data.ripple) {
					data.ripple->paint(p, 0, 0, width());
					if (data.ripple->empty()) {
						data.ripple.reset();
					}
				}
				if (auto icon = (selected ? data.iconOver : data.icon)) {
					icon->paint(p, _st.itemIconPosition, width());
				}
				p.setPen(selected ? _st.itemFgOver : (enabled ? _st.itemFg : _st.itemFgDisabled));
				data.text.drawLeftElided(p, _st.itemPadding.left(), _st.itemPadding.top(), data.textWidth, width());
				if (data.hasSubmenu) {
					const auto left = width() - _st.itemPadding.right() - _st.arrow.width();
					const auto top = (_itemHeight - _st.arrow.height()) / 2;
					if (enabled) {
						_st.arrow.paint(p, left, top, width());
					} else {
						_st.arrow.paint(
							p,
							left,
							top,
							width(),
							_st.itemFgDisabled->c);
					}
				} else if (!data.shortcut.isEmpty()) {
					p.setPen(selected ? _st.itemFgShortcutOver : (enabled ? _st.itemFgShortcut : _st.itemFgShortcutDisabled));
					p.drawTextRight(_st.itemPadding.right(), _st.itemPadding.top(), width(), data.shortcut);
				} else if (data.toggle) {
					auto toggleSize = data.toggle->getSize();
					data.toggle->paint(p, width() - _st.itemPadding.right() - toggleSize.width() + _st.itemToggleShift, (_itemHeight - toggleSize.height()) / 2, width());
				}
			}
		}
		p.translate(0, actionHeight);
	}
}

void Menu::updateSelected(QPoint globalPosition) {
	if (!_mouseSelection) return;

	auto p = mapFromGlobal(globalPosition) - QPoint(0, _st.skip);
	auto selected = -1, top = 0;
	while (top <= p.y() && ++selected < _actions.size()) {
		top += _actions[selected]->isSeparator() ? _separatorHeight : _itemHeight;
	}
	setSelected((selected >= 0 && selected < _actions.size() && _actions[selected]->isEnabled() && !_actions[selected]->isSeparator()) ? selected : -1);
}

void Menu::itemPressed(TriggeredSource source) {
	if (source == TriggeredSource::Mouse && !_mouseSelection) {
		return;
	}
	if (_selected >= 0 && _selected < _actions.size() && _actions[_selected]->isEnabled()) {
		setPressed(_selected);
		if (source == TriggeredSource::Mouse) {
			if (!_actionsData[_pressed].ripple) {
				auto mask = RippleAnimation::rectMask(QSize(width(), _itemHeight));
				_actionsData[_pressed].ripple = std::make_unique<RippleAnimation>(_st.ripple, std::move(mask), [this, selected = _pressed] {
					updateItem(selected);
				});
			}
			_actionsData[_pressed].ripple->add(mapFromGlobal(QCursor::pos()) - QPoint(0, itemTop(_pressed)));
		} else {
			itemReleased(source);
		}
	}
}

void Menu::itemReleased(TriggeredSource source) {
	if (_pressed >= 0 && _pressed < _actions.size()) {
		auto pressed = _pressed;
		setPressed(-1);
		if (source == TriggeredSource::Mouse && _actionsData[pressed].ripple) {
			_actionsData[pressed].ripple->lastStop();
		}
		if (pressed == _selected && _triggeredCallback) {
			_triggeredCallback(_actions[_selected], itemTop(_selected), source);
		}
	}
}

void Menu::keyPressEvent(QKeyEvent *e) {
	auto key = e->key();
	if (!_keyPressDelegate || !_keyPressDelegate(key)) {
		handleKeyPress(key);
	}
}

void Menu::handleKeyPress(int key) {
	if (key == Qt::Key_Enter || key == Qt::Key_Return) {
		itemPressed(TriggeredSource::Keyboard);
		return;
	}
	if (key == (style::RightToLeft() ? Qt::Key_Left : Qt::Key_Right)) {
		if (_selected >= 0 && _actionsData[_selected].hasSubmenu) {
			itemPressed(TriggeredSource::Keyboard);
			return;
		} else if (_selected < 0 && !_actions.empty()) {
			_mouseSelection = false;
			setSelected(0);
		}
	}
	if ((key != Qt::Key_Up && key != Qt::Key_Down) || _actions.empty()) {
		return;
	}

	auto delta = (key == Qt::Key_Down ? 1 : -1), start = _selected;
	if (start < 0 || start >= _actions.size()) {
		start = (delta > 0) ? (_actions.size() - 1) : 0;
	}
	auto newSelected = start;
	do {
		newSelected += delta;
		if (newSelected < 0) {
			newSelected += _actions.size();
		} else if (newSelected >= _actions.size()) {
			newSelected -= _actions.size();
		}
	} while (newSelected != start && (!_actions[newSelected]->isEnabled() || _actions[newSelected]->isSeparator()));

	if (_actions[newSelected]->isEnabled() && !_actions[newSelected]->isSeparator()) {
		_mouseSelection = false;
		setSelected(newSelected);
	}
}

void Menu::clearSelection() {
	_mouseSelection = false;
	setSelected(-1);
}

void Menu::clearMouseSelection() {
	if (_mouseSelection && !_childShown) {
		clearSelection();
	}
}

void Menu::enterEventHook(QEvent *e) {
	QPoint mouse = QCursor::pos();
	if (!rect().marginsRemoved(QMargins(0, _st.skip, 0, _st.skip)).contains(mapFromGlobal(mouse))) {
		clearMouseSelection();
	}
	return TWidget::enterEventHook(e);
}

void Menu::leaveEventHook(QEvent *e) {
	clearMouseSelection();
	return TWidget::leaveEventHook(e);
}

void Menu::setSelected(int selected) {
	if (selected >= _actions.size()) {
		selected = -1;
	}
	if (_selected != selected) {
		updateSelectedItem();
		if (_selected >= 0 && _selected != _pressed && _actionsData[_selected].toggle) {
			_actionsData[_selected].toggle->setStyle(_st.itemToggle);
		}
		_selected = selected;
		if (_selected >= 0 && _actionsData[_selected].toggle && _actions[_selected]->isEnabled()) {
			_actionsData[_selected].toggle->setStyle(_st.itemToggleOver);
		}
		updateSelectedItem();
		if (_activatedCallback) {
			auto source = _mouseSelection ? TriggeredSource::Mouse : TriggeredSource::Keyboard;
			_activatedCallback(
				(_selected >= 0) ? _actions[_selected].get() : nullptr,
				itemTop(_selected),
				source);
		}
	}
}

void Menu::setPressed(int pressed) {
	if (pressed >= _actions.size()) {
		pressed = -1;
	}
	if (_pressed != pressed) {
		if (_pressed >= 0 && _pressed != _selected && _actionsData[_pressed].toggle) {
			_actionsData[_pressed].toggle->setStyle(_st.itemToggle);
		}
		_pressed = pressed;
		if (_pressed >= 0 && _actionsData[_pressed].toggle && _actions[_pressed]->isEnabled()) {
			_actionsData[_pressed].toggle->setStyle(_st.itemToggleOver);
		}
	}
}

int Menu::itemTop(int index) {
	if (index > _actions.size()) {
		index = _actions.size();
	}
	int top = _st.skip;
	for (int i = 0; i < index; ++i) {
		top += _actions.at(i)->isSeparator() ? _separatorHeight : _itemHeight;
	}
	return top;
}

void Menu::updateItem(int index) {
	if (index >= 0 && index < _actions.size()) {
		update(0, itemTop(index), width(), _actions[index]->isSeparator() ? _separatorHeight : _itemHeight);
	}
}

void Menu::updateSelectedItem() {
	updateItem(_selected);
}

void Menu::mouseMoveEvent(QMouseEvent *e) {
	handleMouseMove(e->globalPos());
}

void Menu::handleMouseMove(QPoint globalPosition) {
	auto inner = rect().marginsRemoved(QMargins(0, _st.skip, 0, _st.skip));
	auto localPosition = mapFromGlobal(globalPosition);
	if (inner.contains(localPosition)) {
		_mouseSelection = true;
		updateSelected(globalPosition);
	} else {
		clearMouseSelection();
		if (_mouseMoveDelegate) {
			_mouseMoveDelegate(globalPosition);
		}
	}
}

void Menu::mousePressEvent(QMouseEvent *e) {
	handleMousePress(e->globalPos());
}

void Menu::mouseReleaseEvent(QMouseEvent *e) {
	handleMouseRelease(e->globalPos());
}

void Menu::handleMousePress(QPoint globalPosition) {
	handleMouseMove(globalPosition);
	if (rect().contains(mapFromGlobal(globalPosition))) {
		itemPressed(TriggeredSource::Mouse);
	} else if (_mousePressDelegate) {
		_mousePressDelegate(globalPosition);
	}
}

void Menu::handleMouseRelease(QPoint globalPosition) {
	handleMouseMove(globalPosition);
	itemReleased(TriggeredSource::Mouse);
	if (!rect().contains(mapFromGlobal(globalPosition)) && _mouseReleaseDelegate) {
		_mouseReleaseDelegate(globalPosition);
	}
}

} // namespace Ui