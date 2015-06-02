#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"
#include "Utils.h"

#include <QDir>

///////////////////////////////////////////////////////////////////////////////
SettingsDialog::SettingsDialog(QWidget *parent, Settings &_settings) :
	QDialog(parent, Qt::Sheet),
	ui(new Ui::SettingsDialog),
	settings(&_settings)
{
	ui->setupUi(this);

	CreateLangMap();

	ui->cmbDoubleClickAction->addItem(tr("Diff File"));
	ui->cmbDoubleClickAction->addItem(tr("Open File"));
	ui->cmbDoubleClickAction->addItem(tr("Open Containing Folder"));

	ui->cmbFossilBrowser->addItem(tr("System"));
	ui->cmbFossilBrowser->addItem(tr("Internal"));

	// App Settings
	ui->lineFossilPath->setText(QDir::toNativeSeparators(settings->GetValue(FUEL_SETTING_FOSSIL_PATH).toString()));
	ui->cmbDoubleClickAction->setCurrentIndex(settings->GetValue(FUEL_SETTING_FILE_DBLCLICK).toInt());
	ui->cmbFossilBrowser->setCurrentIndex(settings->GetValue(FUEL_SETTING_WEB_BROWSER).toInt());

	ui->lineCustomFileActionName->setText(settings->GetValue(FUEL_SETTING_FILEACTION_NAME).toString());
	ui->lineCustomFileActionCommand->setText(settings->GetValue(FUEL_SETTING_FILEACTION_COMMAND).toString());

	// Initialize language combo
	foreach(const LangMap &m, langMap)
		ui->cmbActiveLanguage->addItem(m.name);

	QString lang = settings->GetValue(FUEL_SETTING_LANGUAGE).toString();
	// Select current language
	ui->cmbActiveLanguage->setCurrentIndex(
				ui->cmbActiveLanguage->findText(
					LangIdToName(lang)));

}

//-----------------------------------------------------------------------------
SettingsDialog::~SettingsDialog()
{
	delete ui;
}

//-----------------------------------------------------------------------------
bool SettingsDialog::run(QWidget *parent, Settings &settings)
{
	SettingsDialog dlg(parent, settings);
	return dlg.exec() == QDialog::Accepted;
}

//-----------------------------------------------------------------------------
void SettingsDialog::on_buttonBox_accepted()
{
	settings->SetValue(FUEL_SETTING_FOSSIL_PATH, QDir::fromNativeSeparators(ui->lineFossilPath->text()));
	Q_ASSERT(ui->cmbDoubleClickAction->currentIndex()>=FILE_DLBCLICK_ACTION_DIFF && ui->cmbDoubleClickAction->currentIndex()<FILE_DLBCLICK_ACTION_MAX);
	settings->SetValue(FUEL_SETTING_FILE_DBLCLICK, ui->cmbDoubleClickAction->currentIndex());
	settings->SetValue(FUEL_SETTING_WEB_BROWSER, ui->cmbFossilBrowser->currentIndex());

	Q_ASSERT(settings->HasValue(FUEL_SETTING_LANGUAGE));
	QString curr_langid = settings->GetValue(FUEL_SETTING_LANGUAGE).toString();
	QString new_langid = LangNameToId(ui->cmbActiveLanguage->currentText());
	Q_ASSERT(!new_langid.isEmpty());
	settings->SetValue(FUEL_SETTING_LANGUAGE, new_langid);

	if(curr_langid != new_langid)
		QMessageBox::information(this, tr("Restart required"), tr("The language change will take effect after restarting the application"), QMessageBox::Ok);


	Q_ASSERT(settings->HasValue(FUEL_SETTING_FILEACTION_NAME));
	settings->SetValue(FUEL_SETTING_FILEACTION_NAME, ui->lineCustomFileActionName->text().trimmed());
	Q_ASSERT(settings->HasValue(FUEL_SETTING_FILEACTION_COMMAND));
	settings->SetValue(FUEL_SETTING_FILEACTION_COMMAND, QDir::fromNativeSeparators(ui->lineCustomFileActionCommand->text().trimmed()));

	settings->ApplyEnvironment();
}

//-----------------------------------------------------------------------------
void SettingsDialog::on_btnSelectFossil_clicked()
{
	QString path = SelectExe(this, tr("Select Fossil executable"));
	if(!path.isEmpty())
		ui->lineFossilPath->setText(QDir::toNativeSeparators(path));
}

//-----------------------------------------------------------------------------
void SettingsDialog::on_btnClearMessageHistory_clicked()
{
	if(DialogQuery(this, tr("Clear Commit Message History"), tr("Are you sure you want to clear the commit message history?"))==QMessageBox::Yes)
		settings->SetValue(FUEL_SETTING_COMMIT_MSG, QStringList());
}

//-----------------------------------------------------------------------------
void SettingsDialog::CreateLangMap()
{
	langMap.append(LangMap("de_DE", "German (DE)"));
	langMap.append(LangMap("el_GR", "Greek"));
	langMap.append(LangMap("en_US", "English (US)"));
	langMap.append(LangMap("es_ES", "Spanish (ES)"));
	langMap.append(LangMap("fr_FR", "French (FR)"));
	langMap.append(LangMap("ru_RU", "Russian (RU)"));
	langMap.append(LangMap("pt_PT", "Portuguese (PT)"));
}

//-----------------------------------------------------------------------------
QString SettingsDialog::LangIdToName(const QString &id)
{
	foreach(const LangMap &m, langMap)
	{
		if(m.id == id)
			return m.name;
	}

	return "";
}

//-----------------------------------------------------------------------------
QString SettingsDialog::LangNameToId(const QString &name)
{
	foreach(const LangMap &m, langMap)
	{
		if(m.name == name)
			return m.id;
	}

	return "";
}

//-----------------------------------------------------------------------------
void SettingsDialog::on_btnSelectCustomFileActionCommand_clicked()
{
	QString path = SelectExe(this, tr("Select executable"));
	if(!path.isEmpty())
		ui->lineCustomFileActionCommand->setText(QDir::toNativeSeparators(path));
}
