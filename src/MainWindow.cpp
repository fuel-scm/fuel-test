#include "MainWindow.h"
#include "ui_MainWindow.h"
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QInputDialog>
#include <QMimeData>
#include <QProgressBar>
#include <QSettings>
#include <QShortcut>
#include "CommitDialog.h"
#include "FileActionDialog.h"
#include "CloneDialog.h"
#include "Utils.h"

//-----------------------------------------------------------------------------
enum
{
	COLUMN_STATUS,
	COLUMN_FILENAME,
	COLUMN_EXTENSION,
	COLUMN_MODIFIED,
	COLUMN_PATH
};

enum
{
	TAB_LOG,
	TAB_BROWSER
};

enum
{
	REPODIRMODEL_ROLE_PATH = Qt::UserRole+1
};

//-----------------------------------------------------------------------------
typedef QMap<QString, QString> QStringMap;
static QStringMap MakeKeyValues(QStringList lines)
{
	QStringMap res;

	foreach(QString l, lines)
	{
		l = l.trimmed();
		int index = l.indexOf(' ');

		QString key;
		QString value;
		if(index!=-1)
		{
			key = l.left(index).trimmed();
			value = l.mid(index).trimmed();
		}
		else
			key = l;

		res.insert(key, value);
	}
	return res;
}


///////////////////////////////////////////////////////////////////////////////
MainWindow::MainWindow(Settings &_settings, QWidget *parent, QString *workspacePath) :
	QMainWindow(parent),
	ui(new Ui::MainWindow),
	settings(_settings)
{
	ui->setupUi(this);

	QAction *separator = new QAction(this);
	separator->setSeparator(true);

	// fileTableView
	ui->fileTableView->setModel(&getWorkspace().getFileModel());

	ui->fileTableView->addAction(ui->actionDiff);
	ui->fileTableView->addAction(ui->actionHistory);
	ui->fileTableView->addAction(ui->actionOpenFile);
	ui->fileTableView->addAction(ui->actionOpenContaining);
	ui->fileTableView->addAction(separator);
	ui->fileTableView->addAction(ui->actionAdd);
	ui->fileTableView->addAction(ui->actionRevert);
	ui->fileTableView->addAction(ui->actionRename);
	ui->fileTableView->addAction(ui->actionDelete);
	connect( ui->fileTableView,
		SIGNAL( dragOutEvent() ),
		SLOT( onFileViewDragOut() ),
		Qt::DirectConnection );

	QStringList header;
	header << tr("Status") << tr("File") << tr("Extension") << tr("Modified") << tr("Path");
	getWorkspace().getFileModel().setHorizontalHeaderLabels(header);
	getWorkspace().getFileModel().horizontalHeaderItem(COLUMN_STATUS)->setTextAlignment(Qt::AlignCenter);

	// Needed on OSX as the preset value from the GUI editor is not always reflected
	ui->fileTableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);
#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
	ui->fileTableView->horizontalHeader()->setMovable(true);
#else
	ui->fileTableView->horizontalHeader()->setSectionsMovable(true);
#endif
	ui->fileTableView->horizontalHeader()->setStretchLastSection(true);

	// workspaceTreeView
	ui->workspaceTreeView->setModel(&getWorkspace().getDirModel());
	connect( ui->workspaceTreeView->selectionModel(),
		SIGNAL( selectionChanged(const QItemSelection &, const QItemSelection &) ),
		SLOT( onWorkspaceTreeViewSelectionChanged(const QItemSelection &, const QItemSelection &) ),
		Qt::DirectConnection );

	ui->workspaceTreeView->addAction(ui->actionCommit);
	ui->workspaceTreeView->addAction(ui->actionOpenFolder);
	ui->workspaceTreeView->addAction(ui->actionAdd);
	ui->workspaceTreeView->addAction(ui->actionRevert);
	ui->workspaceTreeView->addAction(ui->actionDelete);
	ui->workspaceTreeView->addAction(separator);
	ui->workspaceTreeView->addAction(ui->actionRenameFolder);
	ui->workspaceTreeView->addAction(ui->actionOpenFolder);

	// StashView
	ui->stashTableView->setModel(&getWorkspace().getStashModel());
	ui->stashTableView->addAction(ui->actionApplyStash);
	ui->stashTableView->addAction(ui->actionDiffStash);
	ui->stashTableView->addAction(ui->actionDeleteStash);
	ui->stashTableView->horizontalHeader()->setSortIndicatorShown(false);

	// Recent Workspaces
	// Locate a sequence of two separator actions in file menu
	QList<QAction*> file_actions = ui->menuFile->actions();
	QAction *recent_sep=0;
	for(int i=0; i<file_actions.size(); ++i)
	{
		QAction *act = file_actions[i];
		if(act->isSeparator() && i>0 && file_actions[i-1]->isSeparator())
		{
			recent_sep = act;
			break;
		}
	}
	Q_ASSERT(recent_sep);
	for (int i = 0; i < MAX_RECENT; ++i)
	{
		recentWorkspaceActs[i] = new QAction(this);
		recentWorkspaceActs[i]->setVisible(false);
		connect(recentWorkspaceActs[i], SIGNAL(triggered()), this, SLOT(onOpenRecent()));
		ui->menuFile->insertAction(recent_sep, recentWorkspaceActs[i]);
	}

	// TabWidget
	ui->tabWidget->setCurrentIndex(TAB_LOG);

	// Construct ProgressBar
	progressBar = new QProgressBar();
	progressBar->setMinimum(0);
	progressBar->setMaximum(0);
	progressBar->setMaximumSize(170, 16);
	progressBar->setAlignment(Qt::AlignCenter);
	progressBar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	ui->statusBar->insertPermanentWidget(0, progressBar);
	progressBar->setVisible(false);

#ifdef Q_OS_MACX
	// Native applications on OSX don't have menu icons
	foreach(QAction *a, ui->menuBar->actions())
		a->setIconVisibleInMenu(false);
	foreach(QAction *a, ui->menuFile->actions())
		a->setIconVisibleInMenu(false);
#endif

	abortShortcut = new QShortcut(QKeySequence("Escape"), this);
	abortShortcut->setContext(Qt::ApplicationShortcut);
	abortShortcut->setEnabled(false);
	connect(abortShortcut, SIGNAL(activated()), this, SLOT(onAbort()));

	viewMode = VIEWMODE_TREE;

	uiCallback.init(this);
	// Need to be before applySettings which sets the last workspace
	fossil().Init(&uiCallback);

	applySettings();

	// Apply any explicit workspace path if available
	if(workspacePath && !workspacePath->isEmpty())
		openWorkspace(*workspacePath);

	operationAborted = false;

	rebuildRecent();
}

//------------------------------------------------------------------------------
MainWindow::~MainWindow()
{
	stopUI();
	updateSettings();

	delete ui;
}

//-----------------------------------------------------------------------------
const QString &MainWindow::getCurrentWorkspace()
{
	return fossil().getCurrentWorkspace();
}

//-----------------------------------------------------------------------------
void MainWindow::setCurrentWorkspace(const QString &workspace)
{
	if(workspace.isEmpty())
	{
		fossil().setCurrentWorkspace("");
		return;
	}

	QString new_workspace = QFileInfo(workspace).absoluteFilePath();

	fossil().setCurrentWorkspace(new_workspace);

	addWorkspace(new_workspace);

	if(!QDir::setCurrent(new_workspace))
		QMessageBox::critical(this, tr("Error"), tr("Could not change current directory to '%0'").arg(new_workspace), QMessageBox::Ok );
}

//------------------------------------------------------------------------------
void MainWindow::addWorkspace(const QString &dir)
{
	if(dir.isEmpty())
		return;

	QDir d(dir);
	QString new_workspace = d.absolutePath();

	// Do not add the workspace if it exists already
	if(workspaceHistory.indexOf(new_workspace)!=-1)
		return;

	workspaceHistory.append(new_workspace);
	rebuildRecent();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionRefresh_triggered()
{
	refresh();
}

//------------------------------------------------------------------------------
// Open a fossil file or workspace path. If no checkout is detected offer to
// open the fossil file.
bool MainWindow::openWorkspace(const QString &path)
{
	QFileInfo fi(path);
	QString wkspace = path;

	if(fi.isFile())
	{
		wkspace = fi.absoluteDir().absolutePath();
		QString checkout_file1 = wkspace + PATH_SEPARATOR + FOSSIL_CHECKOUT1;
		QString checkout_file2 = wkspace + PATH_SEPARATOR + FOSSIL_CHECKOUT2;

		if(!(QFileInfo(checkout_file1).exists() || QFileInfo(checkout_file2).exists()) )
		{
			if(QMessageBox::Yes !=DialogQuery(this, tr("Open Workspace"), tr("A workspace does not exist in this folder.\nWould you like to create one here?")))
			{
				wkspace = QFileDialog::getExistingDirectory(
							this,
							tr("Select Workspace Folder"),
							wkspace);

				if(wkspace.isEmpty() || !QDir(wkspace).exists())
					return false;
			}

			// Ok open the repository file
			if(!fossil().openRepository(fi.absoluteFilePath(), wkspace))
			{
				QMessageBox::critical(this, tr("Error"), tr("Could not open repository."), QMessageBox::Ok );
				return false;
			}

		}
		else
		{
			if(!QDir(wkspace).exists())
			{
				QMessageBox::critical(this, tr("Error"), tr("Could not open repository."), QMessageBox::Ok );
				return false;
			}
			setCurrentWorkspace(wkspace);
		}
	}
	else
	{
		if(!QDir(wkspace).exists())
		{
			QMessageBox::critical(this, tr("Error"), tr("Could not open repository."), QMessageBox::Ok );
			return false;
		}
		setCurrentWorkspace(wkspace);
	}

	on_actionClearLog_triggered();
	stopUI();

	// If this repository is not valid, remove it from the history
	if(!refresh())
	{
		setCurrentWorkspace("");
		workspaceHistory.removeAll(path);
		rebuildRecent();
		return false;
	}

	// Select the Root of the tree to update the file view
	selectRootDir();
	return true;
}

//------------------------------------------------------------------------------
void MainWindow::on_actionOpenRepository_triggered()
{
	QString filter(tr("Fossil Files") + QString(" (*." FOSSIL_EXT  " " FOSSIL_CHECKOUT1 " " FOSSIL_CHECKOUT2 ")" ));

	QString path = QFileDialog::getOpenFileName(
		this,
		tr("Open Fossil Repository"),
		QDir::currentPath(),
		filter,
		&filter);

	if(path.isEmpty())
		return;

	openWorkspace(path);
}
//------------------------------------------------------------------------------
void MainWindow::on_actionNewRepository_triggered()
{
	QString filter(tr("Fossil Repositories") + QString(" (*." FOSSIL_EXT ")"));

	// Get Repository file
	QString repo_path = QFileDialog::getSaveFileName(
		this,
		tr("New Fossil Repository"),
		QDir::currentPath(),
		filter,
		&filter);

	if(repo_path.isEmpty())
		return;

	if(QFile::exists(repo_path))
	{
		QMessageBox::critical(this, tr("Error"), tr("A repository file already exists.\nRepository creation aborted."), QMessageBox::Ok );
		return;
	}

	QFileInfo repo_path_info(repo_path);
	Q_ASSERT(repo_path_info.dir().exists());

	// Get Workspace path
	QString wkdir = repo_path_info.absoluteDir().absolutePath();
	if(QMessageBox::Yes != DialogQuery(this, tr("Create Workspace"), tr("Would you like to create a workspace in the same folder?")))
	{
		wkdir = QFileDialog::getExistingDirectory(
			this,
			tr("Select Workspace Folder"),
			wkdir);

		if(wkdir.isEmpty() || !QDir(wkdir).exists())
			return;
	}

	stopUI();
	on_actionClearLog_triggered();

	// Create repository
	QString repo_abs_path = repo_path_info.absoluteFilePath();

	if(!fossil().newRepository(repo_abs_path))
	{
		QMessageBox::critical(this, tr("Error"), tr("Could not create repository."), QMessageBox::Ok );
		return;
	}

	if(!fossil().openRepository(repo_abs_path, wkdir))
	{
		QMessageBox::critical(this, tr("Error"), tr("Could not open repository."), QMessageBox::Ok );
		return;
	}

	// Disable unknown file filter
	if(!ui->actionViewUnknown->isChecked())
		ui->actionViewUnknown->setChecked(true);

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionCloseRepository_triggered()
{
	if(fossil().getRepoStatus()!=REPO_OK)
		return;

	if(QMessageBox::Yes !=DialogQuery(this, tr("Close Workspace"), tr("Are you sure you want to close this workspace?")))
		return;

	// Close Repo
	if(!fossil().closeRepository())
	{
		QMessageBox::critical(this, tr("Error"), tr("Cannot close the workspace.\nAre there still uncommitted changes available?"), QMessageBox::Ok );
		return;
	}

	stopUI();
	setCurrentWorkspace("");
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionCloneRepository_triggered()
{
	QUrl url, url_proxy;
	QString repository;

	if(!CloneDialog::run(this, url, repository, url_proxy))
		return;

	stopUI();

	if(!fossil().cloneRepository(repository, url, url_proxy))
	{
		QMessageBox::critical(this, tr("Error"), tr("Could not clone the repository"), QMessageBox::Ok);
		return;
	}

	openWorkspace(repository);
}

//------------------------------------------------------------------------------
void MainWindow::rebuildRecent()
{
	for(int i = 0; i < MAX_RECENT; ++i)
		recentWorkspaceActs[i]->setVisible(false);

	int enabled_acts = qMin<int>(MAX_RECENT, workspaceHistory.size());

	for(int i = 0; i < enabled_acts; ++i)
	{
		QString text = QString("&%1 %2").arg(i + 1).arg(QDir::toNativeSeparators(workspaceHistory[i]));

		recentWorkspaceActs[i]->setText(text);
		recentWorkspaceActs[i]->setData(workspaceHistory[i]);
		recentWorkspaceActs[i]->setVisible(true);
	}
}

//------------------------------------------------------------------------------
void MainWindow::onOpenRecent()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if(!action)
		return;

	QString workspace = action->data().toString();

	openWorkspace(workspace);
}


//------------------------------------------------------------------------------
void MainWindow::enableActions(bool on)
{
	ui->actionCommit->setEnabled(on);
	ui->actionDiff->setEnabled(on);
	ui->actionAdd->setEnabled(on);
	ui->actionDelete->setEnabled(on);
	ui->actionPush->setEnabled(on);
	ui->actionPull->setEnabled(on);
	ui->actionRename->setEnabled(on);
	ui->actionHistory->setEnabled(on);
	ui->actionFossilUI->setEnabled(on);
	ui->actionRevert->setEnabled(on);
	ui->actionTimeline->setEnabled(on);
	ui->actionOpenFile->setEnabled(on);
	ui->actionOpenContaining->setEnabled(on);
	ui->actionUndo->setEnabled(on);
	ui->actionUpdate->setEnabled(on);
	ui->actionOpenFolder->setEnabled(on);
	ui->actionRenameFolder->setEnabled(on);
	ui->actionNewStash->setEnabled(on);
	ui->actionDeleteStash->setEnabled(on);
	ui->actionDiffStash->setEnabled(on);
	ui->actionApplyStash->setEnabled(on);
}
//------------------------------------------------------------------------------
bool MainWindow::refresh()
{
	QString title = "Fuel";

	// Load repository info
	RepoStatus st = fossil().getRepoStatus();

	if(st==REPO_NOT_FOUND)
	{
		setStatus(tr("No workspace detected."));
		enableActions(false);
		getWorkspace().getFileModel().removeRows(0, getWorkspace().getFileModel().rowCount());
		getWorkspace().getDirModel().clear();
		setWindowTitle(title);
		return false;
	}
	else if(st==REPO_OLD_SCHEMA)
	{
		setStatus(tr("Old repository schema detected. Consider running 'fossil rebuild'"));
		enableActions(false);
		getWorkspace().getFileModel().removeRows(0, getWorkspace().getFileModel().rowCount());
		getWorkspace().getDirModel().clear();
		setWindowTitle(title);
		return true;
	}

	loadFossilSettings();
	scanWorkspace();
	setStatus("");
	enableActions(true);

	if(!fossil().getProjectName().isEmpty())
		title += " - " + fossil().getProjectName();

	setWindowTitle(title);
	return true;
}



//------------------------------------------------------------------------------
void MainWindow::scanWorkspace()
{
	setBusy(true);
	getWorkspace().scanWorkspace(ui->actionViewUnknown->isChecked(),
							ui->actionViewIgnored->isChecked(),
							ui->actionViewModified->isChecked(),
							ui->actionViewUnchanged->isChecked(),
							settings.GetFossilValue(FOSSIL_SETTING_IGNORE_GLOB).toString(),
							uiCallback,
							operationAborted
							);
	updateDirView();
	updateFileView();
	updateStashView();

	setBusy(false);
	setStatus("");
}

//------------------------------------------------------------------------------
static void addPathToTree(QStandardItem &root, const QString &path)
{
	QStringList dirs = path.split('/');
	QStandardItem *parent = &root;

	QString fullpath;
	for(QStringList::iterator it = dirs.begin(); it!=dirs.end(); ++it)
	{
		const QString &dir = *it;
		fullpath += dir;

		// Find the child that matches this subdirectory
		bool found = false;
		for(int r=0; r<parent->rowCount(); ++r)
		{
			QStandardItem *child = parent->child(r, 0);
			Q_ASSERT(child);
			if(child->text() == dir)
			{
				parent = child;
				found = true;
			}
		}

		if(!found) // Generate it
		{
			QStandardItem *child = new QStandardItem(QIcon(":icons/icons/Folder-01.png"), dir);
			child->setData(fullpath); // keep the full path to simplify selection
			parent->appendRow(child);
			parent = child;
		}
		fullpath += '/';
	}
}

//------------------------------------------------------------------------------
void MainWindow::updateDirView()
{
	// Directory View
	getWorkspace().getDirModel().clear();

	QStringList header;
	header << tr("Folders");
	getWorkspace().getDirModel().setHorizontalHeaderLabels(header);

	QStandardItem *root = new QStandardItem(QIcon(":icons/icons/My Documents-01.png"), fossil().getProjectName());
	root->setData(""); // Empty Path
	root->setEditable(false);

	getWorkspace().getDirModel().appendRow(root);
	for(stringset_t::iterator it = getWorkspace().getPaths().begin(); it!=getWorkspace().getPaths().end(); ++it)
	{
		const QString &dir = *it;
		if(dir.isEmpty())
			continue;

		addPathToTree(*root, dir);
	}
	ui->workspaceTreeView->expandToDepth(0);
	ui->workspaceTreeView->sortByColumn(0, Qt::AscendingOrder);
}

//------------------------------------------------------------------------------
void MainWindow::updateFileView()
{
	// Clear content except headers
	getWorkspace().getFileModel().removeRows(0, getWorkspace().getFileModel().rowCount());

	struct { WorkspaceFile::Type type; QString text; const char *icon; }
	stats[] =
	{
		{   WorkspaceFile::TYPE_EDITTED, tr("Edited"), ":icons/icons/Button Blank Yellow-01.png" },
		{   WorkspaceFile::TYPE_UNCHANGED, tr("Unchanged"), ":icons/icons/Button Blank Green-01.png" },
		{   WorkspaceFile::TYPE_ADDED, tr("Added"), ":icons/icons/Button Add-01.png" },
		{   WorkspaceFile::TYPE_DELETED, tr("Deleted"), ":icons/icons/Button Close-01.png" },
		{   WorkspaceFile::TYPE_RENAMED, tr("Renamed"), ":icons/icons/Button Reload-01.png" },
		{   WorkspaceFile::TYPE_MISSING, tr("Missing"), ":icons/icons/Button Help-01.png" },
		{   WorkspaceFile::TYPE_CONFLICTED, tr("Conflicted"), ":icons/icons/Button Blank Red-01.png" },
	};

	QFileIconProvider icon_provider;

	bool display_path = viewMode==VIEWMODE_LIST || selectedDirs.count() > 1;

	size_t item_id=0;
	for(Workspace::filemap_t::iterator it = getWorkspace().getFiles().begin(); it!=getWorkspace().getFiles().end(); ++it)
	{
		const WorkspaceFile &e = *it.value();
		QString path = e.getPath();

		// In Tree mode, filter all items not included in the current dir
		if(viewMode==VIEWMODE_TREE && !selectedDirs.contains(path))
			continue;

		// Status Column
		QString status_text = QString(tr("Unknown"));
		const char *status_icon_path= ":icons/icons/Button Blank Gray-01.png"; // Default icon

		for(size_t t=0; t<COUNTOF(stats); ++t)
		{
			if(e.getType() == stats[t].type)
			{
				status_text = stats[t].text;
				status_icon_path = stats[t].icon;
				break;
			}
		}

		QStandardItem *status = new QStandardItem(QIcon(status_icon_path), status_text);
		status->setToolTip(status_text);
		getWorkspace().getFileModel().setItem(item_id, COLUMN_STATUS, status);

		QFileInfo finfo = e.getFileInfo();
		QIcon icon = icon_provider.icon(finfo);

		QStandardItem *filename_item = 0;
		getWorkspace().getFileModel().setItem(item_id, COLUMN_PATH, new QStandardItem(path));

		if(display_path)
			filename_item = new QStandardItem(icon, QDir::toNativeSeparators(e.getFilePath()));
		else
			filename_item = new QStandardItem(icon, e.getFilename());

		Q_ASSERT(filename_item);
		// Keep the path in the user data
		filename_item->setData(e.getFilePath());
		getWorkspace().getFileModel().setItem(item_id, COLUMN_FILENAME, filename_item);

		getWorkspace().getFileModel().setItem(item_id, COLUMN_EXTENSION, new QStandardItem(finfo.suffix()));
		getWorkspace().getFileModel().setItem(item_id, COLUMN_MODIFIED, new QStandardItem(finfo.lastModified().toString(Qt::SystemLocaleShortDate)));

		++item_id;
	}

	ui->fileTableView->resizeRowsToContents();
}

//------------------------------------------------------------------------------
void MainWindow::updateStashView()
{
	getWorkspace().getStashModel().clear();

	QStringList header;
	header << tr("Stashes");
	getWorkspace().getStashModel().setHorizontalHeaderLabels(header);

	for(stashmap_t::iterator it=getWorkspace().getStashes().begin(); it!=getWorkspace().getStashes().end(); ++it)
	{
		QStandardItem *item = new QStandardItem(it.key());
		item->setToolTip(it.key());
		getWorkspace().getStashModel().appendRow(item);
	}
	ui->stashTableView->resizeColumnsToContents();
	ui->stashTableView->resizeRowsToContents();
}

//------------------------------------------------------------------------------
void MainWindow::log(const QString &text, bool isHTML)
{
	QTextCursor c = ui->textBrowser->textCursor();
	c.movePosition(QTextCursor::End);
	ui->textBrowser->setTextCursor(c);

	if(isHTML)
		ui->textBrowser->insertHtml(text);
	else
		ui->textBrowser->insertPlainText(text);
}

//------------------------------------------------------------------------------
void MainWindow::setStatus(const QString &text)
{
	ui->statusBar->showMessage(text);
}

//------------------------------------------------------------------------------
void MainWindow::on_actionClearLog_triggered()
{
	ui->textBrowser->clear();
}

//------------------------------------------------------------------------------
void MainWindow::applySettings()
{
	QSettings *store = settings.GetStore();

	int num_wks = store->beginReadArray("Workspaces");
	for(int i=0; i<num_wks; ++i)
	{
		store->setArrayIndex(i);
		QString wk = store->value("Path").toString();

		// Skip invalid workspaces
		if(wk.isEmpty() || !QDir(wk).exists())
			continue;

		addWorkspace(wk);

		if(store->contains("Active") && store->value("Active").toBool())
			setCurrentWorkspace(wk);
	}
	store->endArray();

	store->beginReadArray("FileColumns");
	for(int i=0; i<getWorkspace().getFileModel().columnCount(); ++i)
	{
		store->setArrayIndex(i);
		if(store->contains("Width"))
		{
			int width = store->value("Width").toInt();
			ui->fileTableView->setColumnWidth(i, width);
		}

		if(store->contains("Index"))
		{
			int index = store->value("Index").toInt();
			int cur_index = ui->fileTableView->horizontalHeader()->visualIndex(i);
			ui->fileTableView->horizontalHeader()->moveSection(cur_index, index);
		}

	}
	store->endArray();


	if(store->contains("WindowX") && store->contains("WindowY"))
	{
		QPoint _pos;
		_pos.setX(store->value("WindowX").toInt());
		_pos.setY(store->value("WindowY").toInt());
		move(_pos);
	}

	if(store->contains("WindowWidth") && store->contains("WindowHeight"))
	{
		QSize _size;
		_size.setWidth(store->value("WindowWidth").toInt());
		_size.setHeight(store->value("WindowHeight").toInt());
		resize(_size);
	}

	if(store->contains("ViewUnknown"))
		ui->actionViewUnknown->setChecked(store->value("ViewUnknown").toBool());
	if(store->contains("ViewModified"))
		ui->actionViewModified->setChecked(store->value("ViewModified").toBool());
	if(store->contains("ViewUnchanged"))
		ui->actionViewUnchanged->setChecked(store->value("ViewUnchanged").toBool());
	if(store->contains("ViewIgnored"))
		ui->actionViewIgnored->setChecked(store->value("ViewIgnored").toBool());
	if(store->contains("ViewAsList"))
	{
		ui->actionViewAsList->setChecked(store->value("ViewAsList").toBool());
		viewMode = store->value("ViewAsList").toBool()? VIEWMODE_LIST : VIEWMODE_TREE;
	}
	ui->workspaceTreeView->setVisible(viewMode == VIEWMODE_TREE);

	if(store->contains("ViewStash"))
		ui->actionViewStash->setChecked(store->value("ViewStash").toBool());
	ui->stashTableView->setVisible(ui->actionViewStash->isChecked());

}

//------------------------------------------------------------------------------
void MainWindow::updateSettings()
{
	QSettings *store = settings.GetStore();

	store->beginWriteArray("Workspaces", workspaceHistory.size());
	for(int i=0; i<workspaceHistory.size(); ++i)
	{
		store->setArrayIndex(i);
		store->setValue("Path", workspaceHistory[i]);
		if(getCurrentWorkspace() == workspaceHistory[i])
			store->setValue("Active", true);
		else
			store->remove("Active");
	}
	store->endArray();

	store->beginWriteArray("FileColumns", getWorkspace().getFileModel().columnCount());
	for(int i=0; i<getWorkspace().getFileModel().columnCount(); ++i)
	{
		store->setArrayIndex(i);
		store->setValue("Width", ui->fileTableView->columnWidth(i));
		int index = ui->fileTableView->horizontalHeader()->visualIndex(i);
		store->setValue("Index", index);
	}
	store->endArray();

	store->setValue("WindowX", x());
	store->setValue("WindowY", y());
	store->setValue("WindowWidth", width());
	store->setValue("WindowHeight", height());
	store->setValue("ViewUnknown", ui->actionViewUnknown->isChecked());
	store->setValue("ViewModified", ui->actionViewModified->isChecked());
	store->setValue("ViewUnchanged", ui->actionViewUnchanged->isChecked());
	store->setValue("ViewIgnored", ui->actionViewIgnored->isChecked());
	store->setValue("ViewAsList", ui->actionViewAsList->isChecked());
	store->setValue("ViewStash", ui->actionViewStash->isChecked());
}

//------------------------------------------------------------------------------
void MainWindow::selectRootDir()
{
	if(viewMode==VIEWMODE_TREE)
	{
		QModelIndex root_index = ui->workspaceTreeView->model()->index(0, 0);
		ui->workspaceTreeView->selectionModel()->select(root_index, QItemSelectionModel::Select);
	}
}

//------------------------------------------------------------------------------
void MainWindow::fossilBrowse(const QString &fossilUrl)
{
	if(!uiRunning())
		ui->actionFossilUI->activate(QAction::Trigger);

	bool use_internal = settings.GetValue(FUEL_SETTING_WEB_BROWSER).toInt() == 1;

	QUrl url = QUrl(getFossilHttpAddress()+fossilUrl);

	if(use_internal)
	{
		ui->webView->load(url);
		ui->tabWidget->setCurrentIndex(TAB_BROWSER);
	}
	else
		QDesktopServices::openUrl(url);
}
//------------------------------------------------------------------------------
void MainWindow::getSelectionFilenames(QStringList &filenames, int includeMask, bool allIfEmpty)
{
	if(QApplication::focusWidget() == ui->workspaceTreeView)
		getDirViewSelection(filenames, includeMask, allIfEmpty);
	else
		getFileViewSelection(filenames, includeMask, allIfEmpty);
}

//------------------------------------------------------------------------------
void MainWindow::getSelectionPaths(stringset_t &paths)
{
	// Determine the directories selected
	QModelIndexList selection = ui->workspaceTreeView->selectionModel()->selectedIndexes();
	for(QModelIndexList::iterator mi_it = selection.begin(); mi_it!=selection.end(); ++mi_it)
	{
		const QModelIndex &mi = *mi_it;
		QVariant data = getWorkspace().getDirModel().data(mi, REPODIRMODEL_ROLE_PATH);
		paths.insert(data.toString());
	}
}
//------------------------------------------------------------------------------
// Select all workspace files that match the includeMask
void MainWindow::getAllFilenames(QStringList &filenames, int includeMask)
{
	for(Workspace::filemap_t::iterator it=getWorkspace().getFiles().begin(); it!=getWorkspace().getFiles().end(); ++it)
	{
		const WorkspaceFile &e = *(*it);

		// Skip unwanted file types
		if(!(includeMask & e.getType()))
			continue;

		filenames.append(e.getFilePath());
	}
}
//------------------------------------------------------------------------------
void MainWindow::getDirViewSelection(QStringList &filenames, int includeMask, bool allIfEmpty)
{
	// Determine the directories selected
	stringset_t paths;

	QModelIndexList selection = ui->workspaceTreeView->selectionModel()->selectedIndexes();
	if(!(selection.empty() && allIfEmpty))
	{
		getSelectionPaths(paths);
	}

	// Select the actual files form the selected directories
	for(Workspace::filemap_t::iterator it=getWorkspace().getFiles().begin(); it!=getWorkspace().getFiles().end(); ++it)
	{
		const WorkspaceFile &e = *(*it);

		// Skip unwanted file types
		if(!(includeMask & e.getType()))
			continue;

		bool include = true;

		// If we have a limited set of paths to filter, check them
		if(!paths.empty())
			include = false;

		for(stringset_t::iterator p_it=paths.begin(); p_it!=paths.end(); ++p_it)
		{
			const QString &path = *p_it;
			// An empty path is the root folder, so it includes all files
			// If the file's path starts with this, we include id
			if(path.isEmpty() || e.getPath().indexOf(path)==0)
			{
				include = true;
				break;
			}
		}

		if(!include)
			continue;

		filenames.append(e.getFilePath());
	}

}

//------------------------------------------------------------------------------
void MainWindow::getFileViewSelection(QStringList &filenames, int includeMask, bool allIfEmpty)
{
	QModelIndexList selection = ui->fileTableView->selectionModel()->selectedIndexes();
	if(selection.empty() && allIfEmpty)
	{
		ui->fileTableView->selectAll();
		selection = ui->fileTableView->selectionModel()->selectedIndexes();
		ui->fileTableView->clearSelection();
	}

	for(QModelIndexList::iterator mi_it = selection.begin(); mi_it!=selection.end(); ++mi_it)
	{
		const QModelIndex &mi = *mi_it;

		// FIXME: we are being called once per cell of each row
		// but we only need column 1. There must be a better way
		if(mi.column()!=COLUMN_FILENAME)
			continue;

		QVariant data = getWorkspace().getFileModel().data(mi, Qt::UserRole+1);
		QString filename = data.toString();
		Workspace::filemap_t::iterator e_it = getWorkspace().getFiles().find(filename);
		Q_ASSERT(e_it!=getWorkspace().getFiles().end());
		const WorkspaceFile &e = *e_it.value();

		// Skip unwanted files
		if(!(includeMask & e.getType()))
			continue;

		filenames.append(filename);
	}
}
//------------------------------------------------------------------------------
void MainWindow::getStashViewSelection(QStringList &stashNames, bool allIfEmpty)
{
	QModelIndexList selection = ui->stashTableView->selectionModel()->selectedIndexes();
	if(selection.empty() && allIfEmpty)
	{
		ui->stashTableView->selectAll();
		selection = ui->stashTableView->selectionModel()->selectedIndexes();
		ui->stashTableView->clearSelection();
	}

	for(QModelIndexList::iterator mi_it = selection.begin(); mi_it!=selection.end(); ++mi_it)
	{
		const QModelIndex &mi = *mi_it;

		if(mi.column()!=0)
			continue;
		QString name = getWorkspace().getStashModel().data(mi).toString();
		stashNames.append(name);
	}
}

//------------------------------------------------------------------------------
bool MainWindow::diffFile(const QString &repoFile)
{
	return fossil().diffFile(repoFile);
}

//------------------------------------------------------------------------------
void MainWindow::on_actionDiff_triggered()
{
	QStringList selection;
	getSelectionFilenames(selection, WorkspaceFile::TYPE_REPO);

	for(QStringList::iterator it = selection.begin(); it!=selection.end(); ++it)
		if(!diffFile(*it))
			return;
}

//------------------------------------------------------------------------------
bool MainWindow::startUI()
{
	QString port = settings.GetValue(FUEL_SETTING_HTTP_PORT).toString();
	bool started = fossil().startUI(port);
	ui->actionFossilUI->setChecked(started);
	return started;
}
//------------------------------------------------------------------------------
void MainWindow::stopUI()
{
	fossil().stopUI();
	ui->webView->load(QUrl("about:blank"));
	ui->actionFossilUI->setChecked(false);
}

//------------------------------------------------------------------------------
bool MainWindow::uiRunning() const
{
	return fossil().uiRunning();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionFossilUI_triggered()
{
	if(!uiRunning() && ui->actionFossilUI->isChecked())
	{
		startUI();
		fossilBrowse("");
	}
	else
		stopUI();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionQuit_triggered()
{
	close();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionTimeline_triggered()
{
	fossilBrowse("/timeline");
}

//------------------------------------------------------------------------------
void MainWindow::on_actionHistory_triggered()
{
	QStringList selection;
	getSelectionFilenames(selection);

	for(QStringList::iterator it = selection.begin(); it!=selection.end(); ++it)
		fossilBrowse("/finfo?name="+*it);
}

//------------------------------------------------------------------------------
void MainWindow::on_fileTableView_doubleClicked(const QModelIndex &/*index*/)
{
	int action = settings.GetValue(FUEL_SETTING_FILE_DBLCLICK).toInt();
	if(action==FILE_DLBCLICK_ACTION_DIFF)
		on_actionDiff_triggered();
	else if(action==FILE_DLBCLICK_ACTION_OPEN)
		on_actionOpenFile_triggered();
	else if(action==FILE_DLBCLICK_ACTION_OPENCONTAINING)
		on_actionOpenContaining_triggered();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionOpenFile_triggered()
{
	QStringList selection;
	getSelectionFilenames(selection);

	for(QStringList::iterator it = selection.begin(); it!=selection.end(); ++it)
	{
		QDesktopServices::openUrl(QUrl::fromLocalFile(getCurrentWorkspace()+QDir::separator()+*it));
	}
}

//------------------------------------------------------------------------------
void MainWindow::on_actionPush_triggered()
{
	QString remote_url = settings.GetFossilValue(FOSSIL_SETTING_REMOTE_URL).toString();

	if(remote_url.isEmpty() || remote_url == "off")
	{
		QMessageBox::critical(this, tr("Error"), tr("A remote repository has not been specified.\nUse the preferences window to set the remote repostory location"), QMessageBox::Ok );
		return;
	}

	fossil().pushRepository();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionPull_triggered()
{
	QString remote_url = settings.GetFossilValue(FOSSIL_SETTING_REMOTE_URL).toString();

	if(remote_url.isEmpty() || remote_url == "off")
	{
		QMessageBox::critical(this, tr("Error"), tr("A remote repository has not been specified.\nUse the preferences window to set the remote repostory location"), QMessageBox::Ok );
		return;
	}

	fossil().pullRepository();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionCommit_triggered()
{
	QStringList commit_files;
	getSelectionFilenames(commit_files, WorkspaceFile::TYPE_MODIFIED, true);

	if(commit_files.empty())
		return;

	QStringList commit_msgs = settings.GetValue(FUEL_SETTING_COMMIT_MSG).toStringList();

	QString msg;
	bool aborted = !CommitDialog::run(this, tr("Commit Changes"), commit_files, msg, &commit_msgs);

	// Aborted or not we always keep the commit messages.
	// (This has saved me way too many times on TortoiseSVN)
	if(commit_msgs.indexOf(msg)==-1)
	{
		commit_msgs.push_front(msg);
		settings.SetValue(FUEL_SETTING_COMMIT_MSG, commit_msgs);
	}

	if(aborted)
		return;

	// Since via the commit dialog the user can deselect all files
	if(commit_files.empty())
		return;

	// Do commit
	QStringList files;

	// When a subset of files has been selected, explicitely specify each file.
	// Otherwise all files will be implicitly committed by fossil. This is necessary
	// when committing after a merge where fossil thinks that we are trying to do
	// a partial commit which is not permitted.
	QStringList all_modified_files;
	getAllFilenames(all_modified_files, WorkspaceFile::TYPE_MODIFIED);

	if(commit_files.size() != all_modified_files.size())
		files = commit_files;

	fossil().commitFiles(files, msg);
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionAdd_triggered()
{
	// Get unknown files only
	QStringList selection;
	getSelectionFilenames(selection, WorkspaceFile::TYPE_UNKNOWN);

	if(selection.empty())
		return;

	if(!FileActionDialog::run(this, tr("Add files"), tr("The following files will be added.")+"\n"+tr("Are you sure?"), selection))
		return;

	// Do Add
	fossil().addFiles(selection);
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionDelete_triggered()
{
	QStringList repo_files;
	getSelectionFilenames(repo_files, WorkspaceFile::TYPE_REPO);

	QStringList unknown_files;
	getSelectionFilenames(unknown_files, WorkspaceFile::TYPE_UNKNOWN);

	QStringList all_files = repo_files+unknown_files;

	if(all_files.empty())
		return;

	bool remove_local = false;

	if(!FileActionDialog::run(this, tr("Remove files"), tr("The following files will be removed from the repository.")+"\n"+tr("Are you sure?"), all_files, tr("Also delete the local files"), &remove_local ))
		return;

	// Remove repository files
	if(!repo_files.empty())
		fossil().removeFiles(repo_files, remove_local);

	// Remove unknown local files if selected
	if(remove_local)
	{
		for(int i=0; i<unknown_files.size(); ++i)
		{
			QFileInfo fi(getCurrentWorkspace() + QDir::separator() + unknown_files[i]);
			if(fi.exists())
				QFile::remove(fi.filePath());
		}
	}

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionRevert_triggered()
{
	QStringList modified_files;
	getSelectionFilenames(modified_files, WorkspaceFile::TYPE_EDITTED|WorkspaceFile::TYPE_ADDED|WorkspaceFile::TYPE_DELETED|WorkspaceFile::TYPE_MISSING|WorkspaceFile::TYPE_CONFLICTED);

	if(modified_files.empty())
		return;

	if(!FileActionDialog::run(this, tr("Revert files"), tr("The following files will be reverted.")+"\n"+tr("Are you sure?"), modified_files))
		return;

	// Do Revert
	fossil().revertFiles(modified_files);
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionRename_triggered()
{
	QStringList repo_files;
	getSelectionFilenames(repo_files, WorkspaceFile::TYPE_REPO);

	if(repo_files.length()!=1)
		return;

	QFileInfo fi_before(repo_files[0]);

	bool ok = false;
	QString new_name = QInputDialog::getText(this, tr("Rename"), tr("New name"), QLineEdit::Normal, fi_before.filePath(), &ok, Qt::Sheet );
	if(!ok)
		return;

	QFileInfo fi_after(new_name);

	if(fi_after.exists())
	{
		QMessageBox::critical(this, tr("Error"), tr("File '%0' already exists.\nRename aborted.").arg(new_name), QMessageBox::Ok );
		return;
	}

	// Do Rename
	fossil().renameFile(fi_before.filePath(), fi_after.filePath(), true);

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionOpenContaining_triggered()
{
	QStringList selection;
	getSelectionFilenames(selection);

	QString target;

	if(selection.empty())
		target = QDir::toNativeSeparators(getCurrentWorkspace());
	else
	{
		QFileInfo file_info(selection[0]);
		target = QDir::toNativeSeparators(file_info.absoluteDir().absolutePath());
	}

	QUrl url = QUrl::fromLocalFile(target);
	QDesktopServices::openUrl(url);
}

//------------------------------------------------------------------------------
void MainWindow::on_actionUndo_triggered()
{
	// Gather Undo actions
	QStringList res;

	// Do test Undo
	fossil().undoRepository(res, true);

	if(res.length()>0 && res[0]=="No undo or redo is available")
		return;

	if(!FileActionDialog::run(this, tr("Undo"), tr("The following actions will be undone.")+"\n"+tr("Are you sure?"), res))
		return;

	// Do Undo
	fossil().undoRepository(res, false);

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionAbout_triggered()
{
	QString fossil_ver;

	if(fossil().getFossilVersion(fossil_ver))
		fossil_ver = tr("Fossil version %0").arg(fossil_ver) + "\n";

	QString qt_ver = tr("QT version %0").arg(QT_VERSION_STR) + "\n\n";

	QMessageBox::about(this, tr("About Fuel..."),
					   QCoreApplication::applicationName() + " "+ QCoreApplication::applicationVersion() + " " +
						tr("a GUI frontend to the Fossil SCM\n"
							"by Kostas Karanikolas\n"
							"Released under the GNU GPL")+"\n\n" +
					   fossil_ver +
					   qt_ver +
						tr("Icons by Deleket - Jojo Mendoza\n"
							"Available under the CC Attribution Noncommercial No Derivative 3.0 License") + "\n\n" +
						tr("Translations with the help of:") + "\n"
							"stayawake: de_DE\n"
							"djnavas: es_ES\n"
							"Fringale: fr_FR\n"
							"mouse166: ru_RU\n"
							"emansije: pt_PT\n"
					   );
}

//------------------------------------------------------------------------------
void MainWindow::on_actionUpdate_triggered()
{
	QStringList res;

	// Do test update
	if(!fossil().updateRepository(res, true))
		return;

	// Fixme: parse "changes:      None. Already up-to-date" and avoid dialog

	if(res.length()==0)
		return;

	if(!FileActionDialog::run(this, tr("Update"), tr("The following files will be updated.")+"\n"+tr("Are you sure?"), res))
		return;

	// Do update
	fossil().updateRepository(res, false);

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::loadFossilSettings()
{
	// Also retrieve the fossil global settings
	QStringList out;

	if(!fossil().getFossilSettings(out))
		return;

	QStringMap kv = MakeKeyValues(out);

	for(Settings::mappings_t::iterator it=settings.GetMappings().begin(); it!=settings.GetMappings().end(); ++it)
	{
		const QString &name = it.key();
		Settings::Setting::SettingType type = it->Type;

		// Command types we issue directly on fossil

		if(name == FOSSIL_SETTING_REMOTE_URL)
		{
			// Retrieve existing url
			QString url;
			if(fossil().getRemoteUrl(url))
				it.value().Value = url;
			continue;
		}

		Q_ASSERT(type == Settings::Setting::TYPE_FOSSIL_GLOBAL || type == Settings::Setting::TYPE_FOSSIL_LOCAL);

		// Otherwise it must be a fossil setting
		if(!kv.contains(name))
			continue;

		QString value = kv[name];
		if(value.indexOf("(global)") != -1 || value.indexOf("(local)") != -1)
		{
			int i = value.indexOf(" ");
			Q_ASSERT(i!=-1);
			value = value.mid(i).trimmed();

			// Remove quotes if any
			if(value.length()>=2 && value.at(0)=='\"' && value.at(value.length()-1)=='\"')
				value = value.mid(1, value.length()-2);

			it.value().Value = value;
		}
	}
}

//------------------------------------------------------------------------------
void MainWindow::on_actionSettings_triggered()
{
	loadFossilSettings();

	// Run the dialog
	if(!SettingsDialog::run(this, settings))
		return;

	// Apply settings
	for(Settings::mappings_t::iterator it=settings.GetMappings().begin(); it!=settings.GetMappings().end(); ++it)
	{
		const QString &name = it.key();
		Settings::Setting::SettingType type = it.value().Type;

		// Command types we issue directly on fossil
		// FIXME: major uglyness with settings management
		if(name == FOSSIL_SETTING_REMOTE_URL)
		{
			// Run as silent to avoid displaying credentials in the log
			fossil().setRemoteUrl(it.value().Value.toString());
			continue;
		}

		Q_ASSERT(type == Settings::Setting::TYPE_FOSSIL_GLOBAL || type == Settings::Setting::TYPE_FOSSIL_LOCAL);

		QString value = it.value().Value.toString();
		fossil().setFossilSetting(name, value, type == Settings::Setting::TYPE_FOSSIL_GLOBAL);
	}
}

//------------------------------------------------------------------------------
void MainWindow::on_actionViewModified_triggered()
{
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionViewUnchanged_triggered()
{
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionViewUnknown_triggered()
{
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionViewIgnored_triggered()
{
	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionViewAsList_triggered()
{
	viewMode =  ui->actionViewAsList->isChecked() ? VIEWMODE_LIST : VIEWMODE_TREE;
	ui->workspaceTreeView->setVisible(viewMode == VIEWMODE_TREE);
	updateFileView();
}

//------------------------------------------------------------------------------
QString MainWindow::getFossilHttpAddress()
{
	QString port = settings.GetValue(FUEL_SETTING_HTTP_PORT).toString();
	return "http://127.0.0.1:"+port;
}

//------------------------------------------------------------------------------
void MainWindow::onWorkspaceTreeViewSelectionChanged(const QItemSelection &/*selected*/, const QItemSelection &/*deselected*/)
{
	QModelIndexList selection = ui->workspaceTreeView->selectionModel()->selectedIndexes();
	int num_selected = selection.count();

	// Do not modify the selection if nothing is selected
	if(num_selected==0)
		return;

	selectedDirs.clear();

	for(int i=0; i<num_selected; ++i)
	{
		QModelIndex index = selection.at(i);
		QString dir = getWorkspace().getDirModel().data(index, REPODIRMODEL_ROLE_PATH).toString();
		selectedDirs.insert(dir);
	}

	updateFileView();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionOpenFolder_triggered()
{
	const QItemSelection &selection =  ui->workspaceTreeView->selectionModel()->selection();

	if(selection.indexes().count()!=1)
		return;

	QModelIndex index = selection.indexes().at(0);
	on_workspaceTreeView_doubleClicked(index);
}

//------------------------------------------------------------------------------
void MainWindow::on_workspaceTreeView_doubleClicked(const QModelIndex &index)
{
	QString target = getWorkspace().getDirModel().data(index, REPODIRMODEL_ROLE_PATH).toString();
	target = getCurrentWorkspace() + PATH_SEPARATOR + target;

	QUrl url = QUrl::fromLocalFile(target);
	QDesktopServices::openUrl(url);
}

//------------------------------------------------------------------------------
void MainWindow::on_actionRenameFolder_triggered()
{
	stringset_t paths;
	getSelectionPaths(paths);

	if(paths.size()!=1)
		return;

	QString old_path = *paths.begin();

	// Root Node?
	if(old_path.isEmpty())
	{
		// Cannot change the project name via command line
		// so unsupported
		return;
	}

	int dir_start = old_path.lastIndexOf(PATH_SEPARATOR);
	if(dir_start==-1)
		dir_start = 0;
	else
		++dir_start;

	QString old_name = old_path.mid(dir_start);

	bool ok = false;
	QString new_name = QInputDialog::getText(this, tr("Rename Folder"), tr("New name"), QLineEdit::Normal, old_name, &ok, Qt::Sheet);
	if(!ok || old_name==new_name)
		return;

	const char* invalid_tokens[] = {
		"/", "\\", "\\\\", ":", ">", "<", "*", "?", "|", "\"", ".."
	};

	for(size_t i=0; i<COUNTOF(invalid_tokens); ++i)
	{
		if(new_name.indexOf(invalid_tokens[i])!=-1)
		{
			QMessageBox::critical(this, tr("Error"), tr("Cannot rename folder.")+"\n" +tr("Folder name contains invalid characters."));
			return;
		}
	}

	QString new_path = old_path.left(dir_start) + new_name;

	if(getWorkspace().getPaths().contains(new_path))
	{
		QMessageBox::critical(this, tr("Error"), tr("Cannot rename folder.")+"\n" +tr("This folder exists already."));
		return;
	}

	// Collect the files to be moved
	Workspace::filelist_t files_to_move;
	QStringList new_paths;
	QStringList operations;
	foreach(WorkspaceFile *r, getWorkspace().getFiles())
	{
		if(r->getPath().indexOf(old_path)!=0)
			continue;

		files_to_move.append(r);
		QString new_dir = new_path + r->getPath().mid(old_path.length());
		new_paths.append(new_dir);
		QString new_file_path =  new_dir + PATH_SEPARATOR + r->getFilename();
		operations.append(r->getFilePath() + " -> " + new_file_path);
	}

	if(files_to_move.empty())
		return;

	bool move_local = false;
	if(!FileActionDialog::run(this, tr("Rename Folder"), tr("Renaming folder '%0' to '%1'\n"
							  "The following files will be moved in the repository.").arg(old_path, new_path)+"\n"+tr("Are you sure?"),
							  operations,
							  tr("Also move the workspace files"), &move_local)) {
		return;
	}

	// Rename files in fossil
	Q_ASSERT(files_to_move.length() == new_paths.length());
	for(int i=0; i<files_to_move.length(); ++i)
	{
		WorkspaceFile *r = files_to_move[i];
		const QString &new_file_path = new_paths[i] + PATH_SEPARATOR + r->getFilename();

		if(!fossil().renameFile(r->getFilePath(), new_file_path, false))
		{
			log(tr("Move aborted due to errors")+"\n");
			goto _exit;
		}
	}

	if(!move_local)
		goto _exit;

	// First ensure that the target directories exist, and if not make them
	for(int i=0; i<files_to_move.length(); ++i)
	{
		QString target_path = QDir::cleanPath(getCurrentWorkspace() + PATH_SEPARATOR + new_paths[i] + PATH_SEPARATOR);
		QDir target(target_path);

		if(target.exists())
			continue;

		QDir wkdir(getCurrentWorkspace());
		Q_ASSERT(wkdir.exists());

		log(tr("Creating folder '%0'").arg(target_path)+"\n");
		if(!wkdir.mkpath(new_paths[i] + PATH_SEPARATOR + "."))
		{
			QMessageBox::critical(this, tr("Error"), tr("Cannot make target folder '%0'").arg(target_path));
			goto _exit;
		}
	}

	// Now that target directories exist copy files
	for(int i=0; i<files_to_move.length(); ++i)
	{
		WorkspaceFile *r = files_to_move[i];
		QString new_file_path = new_paths[i] + PATH_SEPARATOR + r->getFilename();

		if(QFile::exists(new_file_path))
		{
			QMessageBox::critical(this, tr("Error"), tr("Target file '%0' exists already").arg(new_file_path));
			goto _exit;
		}

		log(tr("Copying file '%0' to '%1'").arg(r->getFilePath(), new_file_path)+"\n");

		if(!QFile::copy(r->getFilePath(), new_file_path))
		{
			QMessageBox::critical(this, tr("Error"), tr("Cannot copy file '%0' to '%1'").arg(r->getFilePath(), new_file_path));
			goto _exit;
		}
	}

	// Finally delete old files
	for(int i=0; i<files_to_move.length(); ++i)
	{
		WorkspaceFile *r = files_to_move[i];

		log(tr("Removing old file '%0'").arg(r->getFilePath())+"\n");

		if(!QFile::exists(r->getFilePath()))
		{
			QMessageBox::critical(this, tr("Error"), tr("Source file '%0' does not exist").arg(r->getFilePath()));
			goto _exit;
		}

		if(!QFile::remove(r->getFilePath()))
		{
			QMessageBox::critical(this, tr("Error"), tr("Cannot remove file '%0'").arg(r->getFilePath()));
			goto _exit;
		}
	}

	log(tr("Folder renamed completed. Don't forget to commit!")+"\n");

_exit:
	refresh();
}

//------------------------------------------------------------------------------
QMenu * MainWindow::createPopupMenu()
{
	return NULL;
}

//------------------------------------------------------------------------------
void MainWindow::on_actionViewStash_triggered()
{
	ui->stashTableView->setVisible(ui->actionViewStash->isChecked());
}

//------------------------------------------------------------------------------
void MainWindow::on_actionNewStash_triggered()
{
	QStringList stashed_files;
	getSelectionFilenames(stashed_files, WorkspaceFile::TYPE_MODIFIED, true);

	if(stashed_files.empty())
		return;

	QString stash_name;
	bool revert = false;
	QString checkbox_text = tr("Revert stashed files");
	if(!CommitDialog::run(this, tr("Stash Changes"), stashed_files, stash_name, 0, true, &checkbox_text, &revert) || stashed_files.empty())
		return;

	stash_name = stash_name.trimmed();

	if(stash_name.indexOf("\"")!=-1 || stash_name.isEmpty())
	{
		QMessageBox::critical(this, tr("Error"), tr("Invalid stash name"));
		return;
	}

	// Check that this stash does not exist
	for(stashmap_t::iterator it=getWorkspace().getStashes().begin(); it!=getWorkspace().getStashes().end(); ++it)
	{
		if(stash_name == it.key())
		{
			QMessageBox::critical(this, tr("Error"), tr("This stash already exists"));
			return;
		}
	}

	// Do Stash
	fossil().stashNew(stashed_files, stash_name, revert);

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionApplyStash_triggered()
{
	QStringList stashes;
	getStashViewSelection(stashes);

	bool delete_stashes = false;
	if(!FileActionDialog::run(this, tr("Apply Stash"), tr("The following stashes will be applied.")+"\n"+tr("Are you sure?"), stashes, tr("Delete after applying"), &delete_stashes))
		return;

	// Apply stashes
	for(QStringList::iterator it=stashes.begin(); it!=stashes.end(); ++it)
	{
		stashmap_t::iterator id_it = getWorkspace().getStashes().find(*it);
		Q_ASSERT(id_it!=getWorkspace().getStashes().end());

		if(!fossil().stashApply(*id_it))
		{
			log(tr("Stash application aborted due to errors")+"\n");
			return;
		}
	}

	// Delete stashes
	for(QStringList::iterator it=stashes.begin(); delete_stashes && it!=stashes.end(); ++it)
	{
		stashmap_t::iterator id_it = getWorkspace().getStashes().find(*it);
		Q_ASSERT(id_it!=getWorkspace().getStashes().end());

		if(!fossil().stashDrop(*id_it))
		{
			log(tr("Stash deletion aborted due to errors")+"\n");
			return;
		}
	}

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionDeleteStash_triggered()
{
	QStringList stashes;
	getStashViewSelection(stashes);

	if(stashes.empty())
		return;

	if(!FileActionDialog::run(this, tr("Delete Stashes"), tr("The following stashes will be deleted.")+"\n"+tr("Are you sure?"), stashes))
		return;

	// Delete stashes
	for(QStringList::iterator it=stashes.begin(); it!=stashes.end(); ++it)
	{
		stashmap_t::iterator id_it = getWorkspace().getStashes().find(*it);
		Q_ASSERT(id_it!=getWorkspace().getStashes().end());

		if(!fossil().stashDrop(*id_it))
		{
			log(tr("Stash deletion aborted due to errors")+"\n");
			return;
		}
	}

	refresh();
}

//------------------------------------------------------------------------------
void MainWindow::on_actionDiffStash_triggered()
{
	QStringList stashes;
	getStashViewSelection(stashes);

	if(stashes.length() != 1)
		return;

	stashmap_t::iterator id_it = getWorkspace().getStashes().find(*stashes.begin());
	Q_ASSERT(id_it!=getWorkspace().getStashes().end());

	// Run diff
	fossil().stashDiff(*id_it);
}

//------------------------------------------------------------------------------
void MainWindow::onFileViewDragOut()
{
	QStringList filenames;
	getFileViewSelection(filenames);

	if(filenames.isEmpty())
		return;

	QList<QUrl> urls;
	foreach(QString f, filenames)
		urls.append(QUrl::fromLocalFile(getCurrentWorkspace()+QDir::separator()+f));

	QMimeData *mime_data = new QMimeData;
	mime_data->setUrls(urls);

	QDrag *drag = new QDrag(this);
	drag->setMimeData(mime_data);
	drag->exec(Qt::CopyAction);
}

//------------------------------------------------------------------------------
void MainWindow::on_textBrowser_customContextMenuRequested(const QPoint &pos)
{
	QMenu *menu = ui->textBrowser->createStandardContextMenu();
	menu->addSeparator();
	menu->addAction(ui->actionClearLog);
	menu->popup(ui->textBrowser->mapToGlobal(pos));
}

//------------------------------------------------------------------------------
void MainWindow::on_fileTableView_customContextMenuRequested(const QPoint &pos)
{
	QPoint gpos = QCursor::pos();
#ifdef Q_OS_WIN
	if(qApp->keyboardModifiers() & Qt::SHIFT)
	{
		ui->fileTableView->selectionModel()->select(ui->fileTableView->indexAt(pos), QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
		QStringList fnames;
		getSelectionFilenames(fnames);

		if(fnames.size()==1)
		{
			QString fname = getCurrentWorkspace() + PATH_SEP + fnames[0];
			fname = QDir::toNativeSeparators(fname);
			if(ShowExplorerMenu((HWND)winId(), fname, gpos))
				refresh();
		}
	}
	else
#else
	Q_UNUSED(pos);
#endif
	{
		QMenu *menu = new QMenu(this);
		menu->addActions(ui->fileTableView->actions());
		menu->popup(gpos);
	}

}

//------------------------------------------------------------------------------
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
	// Ignore drops from the same window
	if(event->source() != this)
		event->acceptProposedAction();
}

//------------------------------------------------------------------------------
void MainWindow::dropEvent(QDropEvent *event)
{
	QList<QUrl> urls = event->mimeData()->urls();

	if(urls.length()==0)
		return;

	// When dropping a folder or a checkout file, open the associated worksspace
	QFileInfo finfo(urls.first().toLocalFile());
	if(finfo.isDir() || finfo.suffix() == FOSSIL_EXT || finfo.fileName() == FOSSIL_CHECKOUT1 || finfo.fileName() == FOSSIL_CHECKOUT2 )
	{
		event->acceptProposedAction();
		openWorkspace(finfo.absoluteFilePath());
	}
	else // Otherwise if not a workspace file and within a workspace, add
	{
		QStringList newfiles;

		Q_FOREACH(const QUrl &url, urls)
		{
			QFileInfo finfo(url.toLocalFile());
			QString abspath = finfo.absoluteFilePath();

			// Within the current workspace ?
			if(abspath.indexOf(getCurrentWorkspace())!=0)
				continue; // skip

			// Remove workspace from full path
			QString wkpath = abspath.right(abspath.length()-getCurrentWorkspace().length()-1);

			newfiles.append(wkpath);
		}

		// Any files to add?
		if(!newfiles.empty())
		{
			if(!FileActionDialog::run(this, tr("Add files"), tr("The following files will be added.")+"\n"+tr("Are you sure?"), newfiles))
				return;

			// Do Add
			fossil().addFiles(newfiles);

			refresh();
		}
	}
}

//------------------------------------------------------------------------------
void MainWindow::setBusy(bool busy)
{
	if(busy)
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	else
		QApplication::restoreOverrideCursor();

	abortShortcut->setEnabled(busy);
	bool enabled = !busy;
	ui->menuBar->setEnabled(enabled);
	ui->mainToolBar->setEnabled(enabled);
	ui->centralWidget->setEnabled(enabled);
}

//------------------------------------------------------------------------------
void MainWindow::onAbort()
{
	operationAborted = true;
	fossil().abortOperation();
	// FIXME: Rename this to something better, Operation Aborted
	log("<br><b>* "+tr("Terminated")+" *</b><br>", true);
}

//------------------------------------------------------------------------------
void MainWindow::fullRefresh()
{
	refresh();
	// Select the Root of the tree to update the file view
	selectRootDir();
}

//------------------------------------------------------------------------------
void MainWindow::MainWinUICallback::logText(const QString& text, bool isHTML)
{
	Q_ASSERT(mainWindow);
	mainWindow->log(text, isHTML);
}

//------------------------------------------------------------------------------
void MainWindow::MainWinUICallback::beginProcess(const QString& text)
{
	Q_ASSERT(mainWindow);
	mainWindow->ui->statusBar->showMessage(text);
	mainWindow->progressBar->setHidden(false);
	QCoreApplication::processEvents();
}

//------------------------------------------------------------------------------
void MainWindow::MainWinUICallback::updateProcess(const QString& text)
{
	Q_ASSERT(mainWindow);
	mainWindow->ui->statusBar->showMessage(text);
	QCoreApplication::processEvents();
}

//------------------------------------------------------------------------------
void MainWindow::MainWinUICallback::endProcess()
{
	Q_ASSERT(mainWindow);
	mainWindow->ui->statusBar->clearMessage();
	mainWindow->progressBar->setHidden(true);
	QCoreApplication::processEvents();
}

//------------------------------------------------------------------------------
QMessageBox::StandardButton MainWindow::MainWinUICallback::Query(const QString &title, const QString &query, QMessageBox::StandardButtons buttons)
{
	return DialogQuery(mainWindow, title, query, buttons);
}
