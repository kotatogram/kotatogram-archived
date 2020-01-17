/*
This file is part of Kotatogram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/kotatogram/kotatogram-desktop/blob/dev/LEGAL
*/
#include "core/kotato_settings.h"

#include "mainwindow.h"
#include "mainwidget.h"
#include "window/window_controller.h"
#include "core/application.h"
#include "base/parse_helper.h"
#include "facades.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QTimer>

namespace KotatoSettings {
namespace {

constexpr auto kWriteJsonTimeout = crl::time(5000);

QString DefaultFilePath() {
	return cWorkingDir() + qsl("tdata/kotato-settings-default.json");
}

QString CustomFilePath() {
	return cWorkingDir() + qsl("tdata/kotato-settings-custom.json");
}

bool DefaultFileIsValid() {
	QFile file(DefaultFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto error = QJsonParseError{ 0, QJsonParseError::NoError };
	const auto document = QJsonDocument::fromJson(
		base::parse::stripComments(file.readAll()),
		&error);
	file.close();

	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return false;
	}
	const auto settings = document.object();

	const auto version = settings.constFind(qsl("version"));
	if (version == settings.constEnd() || (*version).toInt() != AppKotatoVersion) {
		return false;
	}

	return true;
}

void WriteDefaultCustomFile() {
	const auto path = CustomFilePath();
	auto input = QFile(":/misc/default_kotato-settings-custom.json");
	auto output = QFile(path);
	if (input.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly)) {
		output.write(input.readAll());
	}
}

std::unique_ptr<Manager> Data;

} // namespace

Manager::Manager() {
	_jsonWriteTimer.setSingleShot(true);
	connect(&_jsonWriteTimer, SIGNAL(timeout()), this, SLOT(writeTimeout()));
}

void Manager::fill() {
	if (!DefaultFileIsValid()) {
		writeDefaultFile();
	}
	if (!readCustomFile()) {
		WriteDefaultCustomFile();
	}
}

void Manager::write(bool force) {
	if (force && _jsonWriteTimer.isActive()) {
		_jsonWriteTimer.stop();
		writeTimeout();
	} else if (!force && !_jsonWriteTimer.isActive()) {
		_jsonWriteTimer.start(kWriteJsonTimeout);
	}
}

bool Manager::readCustomFile() {
	QFile file(CustomFilePath());
	if (!file.exists()) {
		return false;
	}
	if (!file.open(QIODevice::ReadOnly)) {
		return true;
	}
	auto error = QJsonParseError{ 0, QJsonParseError::NoError };
	const auto document = QJsonDocument::fromJson(
		base::parse::stripComments(file.readAll()),
		&error);
	file.close();

	if (error.error != QJsonParseError::NoError) {
		return true;
	} else if (!document.isObject()) {
		return true;
	}
	const auto settings = document.object();

	if (settings.isEmpty()) {
		return true;
	}

	const auto settingsFontsIt = settings.constFind(qsl("fonts"));

	if (settingsFontsIt != settings.constEnd() && (*settingsFontsIt).isObject()) {
		const auto settingsFonts = (*settingsFontsIt).toObject();

		const auto settingsFontsMain = settingsFonts.constFind(qsl("main"));
		if (settingsFontsMain != settingsFonts.constEnd() && (*settingsFontsMain).isString()) {
			cSetMainFont((*settingsFontsMain).toString());
		}

		const auto settingsFontsSemibold = settingsFonts.constFind(qsl("semibold"));
		if (settingsFontsSemibold != settingsFonts.constEnd() && (*settingsFontsSemibold).isString()) {
			cSetSemiboldFont((*settingsFontsSemibold).toString());
		}

		const auto settingsFontsSemiboldIsBold = settingsFonts.constFind(qsl("semibold_is_bold"));
		if (settingsFontsSemiboldIsBold != settingsFonts.constEnd() && (*settingsFontsSemiboldIsBold).isBool()) {
			cSetSemiboldFontIsBold((*settingsFontsSemiboldIsBold).toBool());
		}

		const auto settingsFontsMonospace = settingsFonts.constFind(qsl("monospaced"));
		if (settingsFontsMonospace != settingsFonts.constEnd() && (*settingsFontsMonospace).isString()) {
			cSetMonospaceFont((*settingsFontsMonospace).toString());
		}
	}
	return true;
}

void Manager::writeDefaultFile() {
	auto file = QFile(DefaultFilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	const char *defaultHeader = R"HEADER(
// This is a list of default options for Kotatogram Desktop
// Please don't modify it, its content is not used in any way
// You can place your own options in the 'kotato-settings-custom.json' file

)HEADER";
	file.write(defaultHeader);

	auto settings = QJsonObject();
	settings.insert(qsl("version"), QString::number(AppKotatoVersion));

	auto settingsFonts = QJsonObject();
	settingsFonts.insert(qsl("main"), qsl("Open Sans"));
	settingsFonts.insert(qsl("semibold"), qsl("Open Sans Semibold"));
	settingsFonts.insert(qsl("semibold_is_bold"), false);
	settingsFonts.insert(qsl("monospaced"), qsl("Consolas"));

	settings.insert(qsl("fonts"), settingsFonts);
	auto document = QJsonDocument();
	document.setObject(settings);
	file.write(document.toJson(QJsonDocument::Indented));
}

void Manager::writeCurrentSettings() {
	auto file = QFile(CustomFilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	if (_jsonWriteTimer.isActive()) {
		writing();
	}
	const char *customHeader = R"HEADER(
// This file was automatically generated from current settings
// It's better to edit it with app closed, so there will be no rewrites
// You should restart app to see changes

)HEADER";
	file.write(customHeader);

	auto settings = QJsonObject();

	auto settingsFonts = QJsonObject();
	
	if (!cMainFont().isEmpty()) {
		settingsFonts.insert(qsl("main"), cMainFont());
	}

	if (!cSemiboldFont().isEmpty()) {
		settingsFonts.insert(qsl("semibold"), cSemiboldFont());
	}

	if (!cMonospaceFont().isEmpty()) {
		settingsFonts.insert(qsl("monospaced"), cMonospaceFont());
	}

	settingsFonts.insert(qsl("semibold_is_bold"), cSemiboldFontIsBold());

	settings.insert(qsl("fonts"), settingsFonts);

	auto document = QJsonDocument();
	document.setObject(settings);
	file.write(document.toJson(QJsonDocument::Indented));
}

void Manager::writeTimeout() {
	writeCurrentSettings();
}

void Manager::writing() {
	_jsonWriteTimer.stop();
}

void Start() {
	if (Data) return;

	Data = std::make_unique<Manager>();
	Data->fill();
}

void Write() {
	if (!Data) return;

	Data->write();
}

void Finish() {
	if (!Data) return;

	Data->write(true);
}

} // namespace KotatoSettings
