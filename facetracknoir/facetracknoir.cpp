/********************************************************************************
* FaceTrackNoIR		This program is a private project of the some enthusiastic	*
*					gamers from Holland, who don't like to pay much for			*
*					head-tracking.												*
*																				*
* Copyright (C) 2011	Wim Vriend (Developing)									*
*						Ron Hendriks (Researching and Testing)					*
*																				*
* Homepage																		*
*																				*
* This program is free software; you can redistribute it and/or modify it		*
* under the terms of the GNU General Public License as published by the			*
* Free Software Foundation; either version 3 of the License, or (at your		*
* option) any later version.													*
*																				*
* This program is distributed in the hope that it will be useful, but			*
* WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY	*
* or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for	*
* more details.																	*
*																				*
* You should have received a copy of the GNU General Public License along		*
* with this program; if not, see <http://www.gnu.org/licenses/>.				*
*********************************************************************************/
#include "facetracknoir.h"
#include "shortcuts.h"
#include "tracker.h"
#include "curve-config.h"
#include "opentrack-version.h"
#include <QDebug>

#if defined(_WIN32)
#   include <windows.h>
#   include <dshow.h>
#endif

#if defined(__APPLE__)
#   define SONAME "dylib"
#elif defined(_WIN32)
#   define SONAME "dll"
#else
#   define SONAME "so"
#endif

#include <iostream>

#ifdef _MSC_VER
#   define LIB_PREFIX ""
#else
#   define LIB_PREFIX "lib"
#endif

#if defined(__unix) || defined(__linux) || defined(__APPLE__)
#   include <unistd.h>
#endif

static bool get_metadata(DynamicLibrary* lib, QString& longName, QIcon& icon)
{
    Metadata* meta;
    if (!lib->Metadata || ((meta = lib->Metadata()), !meta))
        return false;
    meta->getFullName(&longName);
    meta->getIcon(&icon);
    delete meta;
    return true;
}

static void fill_combobox(const QString& filter, QList<DynamicLibrary*>& list, QComboBox* cbx, QComboBox* cbx2)
{
    QDir settingsDir( QCoreApplication::applicationDirPath() );
    QStringList filenames = settingsDir.entryList( QStringList() << (LIB_PREFIX + filter + SONAME), QDir::Files, QDir::Name );
    for ( int i = 0; i < filenames.size(); i++) {
        QIcon icon;
        QString longName;
        QString str = filenames.at(i);
        DynamicLibrary* lib = new DynamicLibrary(str);
        qDebug() << "Loading" << str;
        std::cout.flush();
        if (!get_metadata(lib, longName, icon))
        {
            delete lib;
            continue;
        }
        list.push_back(lib);
        cbx->addItem(icon, longName);
        if (cbx2)
            cbx2->addItem(icon, longName);
    }
}

FaceTrackNoIR::FaceTrackNoIR(QWidget *parent) :
    QMainWindow(parent),
#if defined(_WIN32)
    keybindingWorker(NULL),
#else
    keyCenter(this),
    keyToggle(this),
#endif
    timUpdateHeadPose(this),
    pTrackerDialog(NULL),
    pSecondTrackerDialog(NULL),
    pProtocolDialog(NULL),
    pFilterDialog(NULL),
    kbd_quit(QKeySequence("Ctrl+Q"), this),
    looping(false)
{	
    ui.setupUi(this);
    setFixedSize(size());

	_keyboard_shortcuts = 0;
	_curve_config = 0;

	tracker = 0;

    QDir::setCurrent(QCoreApplication::applicationDirPath());

    connect(ui.btnLoad, SIGNAL(clicked()), this, SLOT(open()));
    connect(ui.btnSave, SIGNAL(clicked()), this, SLOT(save()));
    connect(ui.btnSaveAs, SIGNAL(clicked()), this, SLOT(saveAs()));

    connect(ui.btnEditCurves, SIGNAL(clicked()), this, SLOT(showCurveConfiguration()));
    connect(ui.btnShortcuts, SIGNAL(clicked()), this, SLOT(showKeyboardShortcuts()));
    connect(ui.btnShowEngineControls, SIGNAL(clicked()), this, SLOT(showTrackerSettings()));
    connect(ui.btnShowSecondTrackerSettings, SIGNAL(clicked()), this, SLOT(showSecondTrackerSettings()));
    connect(ui.btnShowServerControls, SIGNAL(clicked()), this, SLOT(showServerControls()));
    connect(ui.btnShowFilterControls, SIGNAL(clicked()), this, SLOT(showFilterControls()));

    connect(ui.chkInvertYaw, SIGNAL(stateChanged(int)), this, SLOT(setInvertYaw(int)));
    connect(ui.chkInvertRoll, SIGNAL(stateChanged(int)), this, SLOT(setInvertRoll(int)));
    connect(ui.chkInvertPitch, SIGNAL(stateChanged(int)), this, SLOT(setInvertPitch(int)));
    connect(ui.chkInvertX, SIGNAL(stateChanged(int)), this, SLOT(setInvertX(int)));
    connect(ui.chkInvertY, SIGNAL(stateChanged(int)), this, SLOT(setInvertY(int)));
    connect(ui.chkInvertZ, SIGNAL(stateChanged(int)), this, SLOT(setInvertZ(int)));

    connect(ui.btnStartTracker, SIGNAL(clicked()), this, SLOT(startTracker()));
    connect(ui.btnStopTracker, SIGNAL(clicked()), this, SLOT(stopTracker()));

    GetCameraNameDX();

    ui.cbxSecondTrackerSource->addItem(QIcon(), "None");
    dlopen_filters.push_back((DynamicLibrary*) NULL);
    ui.iconcomboFilter->addItem(QIcon(), "None");

    fill_combobox("opentrack-proto-*.", dlopen_protocols, ui.iconcomboProtocol, NULL);
    fill_combobox("opentrack-tracker-*.", dlopen_trackers, ui.iconcomboTrackerSource, ui.cbxSecondTrackerSource);
    fill_combobox("opentrack-filter-*.", dlopen_filters, ui.iconcomboFilter, NULL);

    connect(ui.iconcomboProfile, SIGNAL(currentIndexChanged(int)), this, SLOT(profileSelected(int)));
    connect(&timUpdateHeadPose, SIGNAL(timeout()), this, SLOT(showHeadPose()));

    loadSettings();

#ifndef _WIN32
    connect(&keyCenter, SIGNAL(activated()), this, SLOT(shortcutRecentered()));
    connect(&keyToggle, SIGNAL(activated()), this, SLOT(shortcutToggled()));
#endif

    connect(&kbd_quit, SIGNAL(activated()), this, SLOT(exit()));
    kbd_quit.setEnabled(true);
}

FaceTrackNoIR::~FaceTrackNoIR() {

	stopTracker();
    save();
    if (Libraries)
        delete Libraries;
}

QFrame* FaceTrackNoIR::get_video_widget() {
    return ui.video_frame;
}

void FaceTrackNoIR::GetCameraNameDX() {
#if defined(_WIN32)
	ui.cameraName->setText("No video-capturing device was found in your system: check if it's connected!");

	// Create the System Device Enumerator.
	HRESULT hr;
	ICreateDevEnum *pSysDevEnum = NULL;
	hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void **)&pSysDevEnum);
	if (FAILED(hr))
	{
		qDebug() << "GetWDM says: CoCreateInstance Failed!";
		return;
	}

	qDebug() << "GetWDM says: CoCreateInstance succeeded!";
	
	// Obtain a class enumerator for the video compressor category.
	IEnumMoniker *pEnumCat = NULL;
	hr = pSysDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnumCat, 0);

	if (hr == S_OK) {
		qDebug() << "GetWDM says: CreateClassEnumerator succeeded!";

		IMoniker *pMoniker = NULL;
		ULONG cFetched;
		if (pEnumCat->Next(1, &pMoniker, &cFetched) == S_OK) {
			IPropertyBag *pPropBag;
			hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag, (void **)&pPropBag);
			if (SUCCEEDED(hr))	{
				VARIANT varName;
				VariantInit(&varName);
				hr = pPropBag->Read(L"FriendlyName", &varName, 0);
				if (SUCCEEDED(hr))
				{
					QString str((QChar*)varName.bstrVal, wcslen(varName.bstrVal));
					qDebug() << "GetWDM says: Moniker found:" << str;
					ui.cameraName->setText(str);
				}
				VariantClear(&varName);

				pPropBag->Release();
			}
			pMoniker->Release();
		}
		pEnumCat->Release();
	}
	pSysDevEnum->Release();
#else
    for (int i = 0; i < 16; i++) {
        char buf[128];
        sprintf(buf, "/dev/video%d", i);
        if (access(buf, R_OK | W_OK) == 0) {
            ui.cameraName->setText(QString(buf));
            break;
        }
    }
#endif
}

void FaceTrackNoIR::open() {
     QFileDialog dialog(this);
     dialog.setFileMode(QFileDialog::ExistingFile);
     
	 QString fileName = dialog.getOpenFileName(
								this,
                                 tr("Select one FTNoir settings file"),
								 QCoreApplication::applicationDirPath() + "/settings/",
                                 tr("Settings file (*.ini);;All Files (*)"),
                                               NULL);

	if (! fileName.isEmpty() ) {
        {
            QSettings settings("opentrack");
            settings.setValue ("SettingsFile", QFileInfo(fileName).absoluteFilePath());
        }
		loadSettings();
    }
}

void FaceTrackNoIR::save() {
	QSettings settings("opentrack");

	QString currentFile = settings.value ( "SettingsFile", QCoreApplication::applicationDirPath() + "/settings/default.ini" ).toString();
	QSettings iniFile( currentFile, QSettings::IniFormat );	

	iniFile.beginGroup ( "Tracking" );

	iniFile.setValue ( "invertYaw", ui.chkInvertYaw->isChecked() );
	iniFile.setValue ( "invertPitch", ui.chkInvertPitch->isChecked() );
	iniFile.setValue ( "invertRoll", ui.chkInvertRoll->isChecked() );
	iniFile.setValue ( "invertX", ui.chkInvertX->isChecked() );
	iniFile.setValue ( "invertY", ui.chkInvertY->isChecked() );
	iniFile.setValue ( "invertZ", ui.chkInvertZ->isChecked() );
	iniFile.endGroup ();

	iniFile.beginGroup ( "GameProtocol" );
    {
        DynamicLibrary* proto = dlopen_protocols.value( ui.iconcomboProtocol->currentIndex(), (DynamicLibrary*) NULL);
        iniFile.setValue ( "DLL",  proto == NULL ? "" : proto->filename);
    }
	iniFile.endGroup ();

	iniFile.beginGroup ( "TrackerSource" );
    {
        DynamicLibrary* tracker = dlopen_trackers.value( ui.iconcomboTrackerSource->currentIndex(), (DynamicLibrary*) NULL);
        iniFile.setValue ( "DLL",  tracker == NULL ? "" : tracker->filename);
    }
    {
        DynamicLibrary* tracker = dlopen_trackers.value( ui.cbxSecondTrackerSource->currentIndex() - 1, (DynamicLibrary*) NULL);
        iniFile.setValue ( "2ndDLL",  tracker == NULL ? "" : tracker->filename);
    }
	iniFile.endGroup ();

	iniFile.beginGroup ( "Filter" );
    {
        DynamicLibrary* filter = dlopen_filters.value( ui.iconcomboFilter->currentIndex(), (DynamicLibrary*) NULL);
        iniFile.setValue ( "DLL",  filter == NULL ? "" : filter->filename);
    }
	iniFile.endGroup ();

#if defined(__unix) || defined(__linux)
    QByteArray bytes = QFile::encodeName(currentFile);
    const char* filename_as_asciiz = bytes.constData();

    if (access(filename_as_asciiz, R_OK | W_OK))
    {
        QMessageBox::warning(this, "Something went wrong", "Check permissions and ownership for your .ini file!", QMessageBox::Ok, QMessageBox::NoButton);
    }
#endif
}

void FaceTrackNoIR::saveAs()
{
	QSettings settings("opentrack");
	QString oldFile = settings.value ( "SettingsFile", QCoreApplication::applicationDirPath() + "/settings/default.ini" ).toString();

	QString fileName = QFileDialog::getSaveFileName(this, tr("Save file"),
													oldFile,
//													QCoreApplication::applicationDirPath() + "/settings",
													tr("Settings file (*.ini);;All Files (*)"));
	if (!fileName.isEmpty()) {

		QFileInfo newFileInfo ( fileName );
		if ((newFileInfo.exists()) && (oldFile != fileName)) {
			QFile newFileFile ( fileName );
			newFileFile.remove();
		}

		QFileInfo oldFileInfo ( oldFile );
		if (oldFileInfo.exists()) {
			QFile oldFileFile ( oldFile );
			oldFileFile.copy( fileName );
		}

		settings.setValue ("SettingsFile", fileName);
		save();

		loadSettings();
	}
}

void FaceTrackNoIR::loadSettings() {
    if (looping)
        return;
    looping = true;
	qDebug() << "loadSettings says: Starting ";
    QSettings settings("opentrack");

	QString currentFile = settings.value ( "SettingsFile", QCoreApplication::applicationDirPath() + "/settings/default.ini" ).toString();
    qDebug() << "Config file now" << currentFile;
	QSettings iniFile( currentFile, QSettings::IniFormat );	

    QFileInfo pathInfo ( currentFile );
    setWindowTitle(QString( OPENTRACK_VERSION " :: ") + pathInfo.fileName());

	QDir settingsDir( pathInfo.dir() );
    QStringList filters;
    filters << "*.ini";
	iniFileList.clear();
	iniFileList = settingsDir.entryList( filters, QDir::Files, QDir::Name );
	
	ui.iconcomboProfile->clear();
	for ( int i = 0; i < iniFileList.size(); i++) {
        ui.iconcomboProfile->addItem(QIcon(":/images/settings16.png"), iniFileList.at(i));
		if (iniFileList.at(i) == pathInfo.fileName()) {
            ui.iconcomboProfile->setItemIcon(i, QIcon(":/images/settingsopen16.png"));
			ui.iconcomboProfile->setCurrentIndex( i );
		}
	}

	qDebug() << "loadSettings says: iniFile = " << currentFile;

	iniFile.beginGroup ( "Tracking" );
    ui.chkInvertYaw->setChecked (iniFile.value ( "invertYaw", 0 ).toBool());
	ui.chkInvertPitch->setChecked (iniFile.value ( "invertPitch", 0 ).toBool());
	ui.chkInvertRoll->setChecked (iniFile.value ( "invertRoll", 0 ).toBool());
	ui.chkInvertX->setChecked (iniFile.value ( "invertX", 0 ).toBool());
	ui.chkInvertY->setChecked (iniFile.value ( "invertY", 0 ).toBool());
	ui.chkInvertZ->setChecked (iniFile.value ( "invertZ", 0 ).toBool());
	iniFile.endGroup ();

	iniFile.beginGroup ( "GameProtocol" );
    QString selectedProtocolName = iniFile.value ( "DLL", "" ).toString();
    iniFile.endGroup ();

    for ( int i = 0; i < dlopen_protocols.size(); i++) {
        if (dlopen_protocols.at(i)->filename.compare( selectedProtocolName, Qt::CaseInsensitive ) == 0) {
			ui.iconcomboProtocol->setCurrentIndex( i );
			break;
		}
	}

    iniFile.beginGroup ( "TrackerSource" );
    QString selectedTrackerName = iniFile.value ( "DLL", "" ).toString();
    qDebug() << "loadSettings says: selectedTrackerName = " << selectedTrackerName;
    QString secondTrackerName = iniFile.value ( "2ndDLL", "None" ).toString();
    qDebug() << "loadSettings says: secondTrackerName = " << secondTrackerName;
    iniFile.endGroup ();

    for ( int i = 0; i < dlopen_trackers.size(); i++) {
        DynamicLibrary* foo = dlopen_trackers.at(i);
        if (foo && foo->filename.compare( selectedTrackerName, Qt::CaseInsensitive ) == 0) {
			ui.iconcomboTrackerSource->setCurrentIndex( i );
		}
        if (foo && foo->filename.compare( secondTrackerName, Qt::CaseInsensitive ) == 0) {
            ui.cbxSecondTrackerSource->setCurrentIndex( i + 1 );
		}
	}

	iniFile.beginGroup ( "Filter" );
    QString selectedFilterName = iniFile.value ( "DLL", "" ).toString();
	qDebug() << "createIconGroupBox says: selectedFilterName = " << selectedFilterName;
	iniFile.endGroup ();

    for ( int i = 0; i < dlopen_filters.size(); i++) {
        DynamicLibrary* foo = dlopen_filters.at(i);
        if (foo && foo->filename.compare( selectedFilterName, Qt::CaseInsensitive ) == 0) {
            ui.iconcomboFilter->setCurrentIndex( i );
			break;
		}
	}

    CurveConfigurationDialog* ccd;

    if (!_curve_config)
    {
        ccd = new CurveConfigurationDialog( this, this );
        _curve_config = ccd;
    } else {
        ccd = dynamic_cast<CurveConfigurationDialog*>(_curve_config);
    }

    ccd->loadSettings();

    looping = false;
}

void FaceTrackNoIR::startTracker( ) {	
    bindKeyboardShortcuts();

	ui.iconcomboProfile->setEnabled ( false );
	ui.btnLoad->setEnabled ( false );
	ui.btnSave->setEnabled ( false );
	ui.btnSaveAs->setEnabled ( false );
	ui.btnShowFilterControls->setEnabled ( true );

    if (Libraries)
        delete Libraries;
    Libraries = new SelectedLibraries(this);

    if (!Libraries->correct)
    {
        QMessageBox::warning(this, "Something went wrong", "Tracking can't be initialized, probably protocol prerequisites missing", QMessageBox::Ok, QMessageBox::NoButton);
        stopTracker();
        return;
    }
    
#if defined(_WIN32)
    keybindingWorker = new KeybindingWorker(*this, keyCenter, keyToggle);
    keybindingWorker->start();
#endif

    if (tracker) {
        tracker->wait();
        delete tracker;
    }

    QSettings settings("opentrack");
    QString currentFile = settings.value ( "SettingsFile", QCoreApplication::applicationDirPath() + "/settings/default.ini" ).toString();
    QSettings iniFile( currentFile, QSettings::IniFormat );	

    for (int i = 0; i < 6; i++)
    {
        axis(i).curve.loadSettings(iniFile);
        axis(i).curveAlt.loadSettings(iniFile);
    }

    static const char* names[] = {
        "tx_alt",
        "ty_alt",
        "tz_alt",
        "rx_alt",
        "ry_alt",
        "rz_alt",
    };

    static const char* invert_names[] = {
        "invertX",
        "invertY",
        "invertZ",
        "invertYaw",
        "invertPitch",
        "invertRoll"
    };

    iniFile.beginGroup("Tracking");

    for (int i = 0; i < 6; i++) {
        axis(i).altp = iniFile.value(names[i], false).toBool();
        axis(i).invert = iniFile.value(invert_names[i], false).toBool() ? 1 : -1;
    }

    tracker = new Tracker ( this );

    tracker->compensate = iniFile.value("compensate", true).toBool();
    tracker->tcomp_rz = iniFile.value("tcomp-rz", false).toBool();

    iniFile.endGroup();

    tracker->setInvertAxis(Yaw, ui.chkInvertYaw->isChecked() );
    tracker->setInvertAxis(Pitch, ui.chkInvertPitch->isChecked() );
    tracker->setInvertAxis(Roll, ui.chkInvertRoll->isChecked() );
    tracker->setInvertAxis(TX, ui.chkInvertX->isChecked() );
    tracker->setInvertAxis(TY, ui.chkInvertY->isChecked() );
    tracker->setInvertAxis(TZ, ui.chkInvertZ->isChecked() );

    if (pTrackerDialog && Libraries->pTracker) {
        pTrackerDialog->registerTracker( Libraries->pTracker );
	}
    
    if (pFilterDialog && Libraries->pFilter)
        pFilterDialog->registerFilter(Libraries->pFilter);
    
    tracker->start();

    ui.video_frame->show();

	ui.btnStartTracker->setEnabled ( false );
	ui.btnStopTracker->setEnabled ( true );

	ui.iconcomboTrackerSource->setEnabled ( false );
	ui.cbxSecondTrackerSource->setEnabled ( false );
	ui.iconcomboProtocol->setEnabled ( false );
    ui.btnShowServerControls->setEnabled ( false );
	ui.iconcomboFilter->setEnabled ( false );

	GetCameraNameDX();

    timUpdateHeadPose.start(50);
}

void FaceTrackNoIR::stopTracker( ) {	
    ui.game_name->setText("Not connected");
#if defined(_WIN32)
    if (keybindingWorker)
    {
        keybindingWorker->should_quit = true;
        keybindingWorker->wait();
        delete keybindingWorker;
        keybindingWorker = NULL;
    }
#endif
	timUpdateHeadPose.stop();
    ui.pose_display->rotateBy(0, 0, 0);

    if (pTrackerDialog) {
        pTrackerDialog->unRegisterTracker();
    }
    if (pProtocolDialog) {
        pProtocolDialog->unRegisterProtocol();
    }
    if (pFilterDialog)
        pFilterDialog->unregisterFilter();

    if ( tracker ) {
        qDebug() << "Done with tracking";
        tracker->should_quit = true;
        tracker->wait();

		qDebug() << "stopTracker says: Deleting tracker!";
		delete tracker;
		qDebug() << "stopTracker says: Tracker deleted!";
		tracker = 0;
        if (Libraries) {
            delete Libraries;
            Libraries = NULL;
        }
	}
	ui.btnStartTracker->setEnabled ( true );
	ui.btnStopTracker->setEnabled ( false );
	ui.iconcomboProtocol->setEnabled ( true );
	ui.iconcomboTrackerSource->setEnabled ( true );
	ui.cbxSecondTrackerSource->setEnabled ( true );
	ui.iconcomboFilter->setEnabled ( true );

	ui.btnShowServerControls->setEnabled ( true );
	ui.video_frame->hide();

	ui.iconcomboProfile->setEnabled ( true );
	ui.btnLoad->setEnabled ( true );
	ui.btnSave->setEnabled ( true );
	ui.btnSaveAs->setEnabled ( true );
	ui.btnShowFilterControls->setEnabled ( true );
}

void FaceTrackNoIR::setInvertAxis(Axis axis, int invert ) {
    if (tracker)
        tracker->setInvertAxis (axis, (invert != 0)?true:false );
}

void FaceTrackNoIR::showHeadPose() {
    double newdata[6];

    tracker->getHeadPose(newdata);
    ui.lcdNumX->display(newdata[TX]);
    ui.lcdNumY->display(newdata[TY]);
    ui.lcdNumZ->display(newdata[TZ]);


    ui.lcdNumRotX->display(newdata[Yaw]);
    ui.lcdNumRotY->display(newdata[Pitch]);
    ui.lcdNumRotZ->display(newdata[Roll]);

    tracker->getOutputHeadPose(newdata);

    ui.pose_display->rotateBy(newdata[Yaw], newdata[Roll], newdata[Pitch]);

    ui.lcdNumOutputPosX->display(newdata[TX]);
    ui.lcdNumOutputPosY->display(newdata[TY]);
    ui.lcdNumOutputPosZ->display(newdata[TZ]);

    ui.lcdNumOutputRotX->display(newdata[Yaw]);
    ui.lcdNumOutputRotY->display(newdata[Pitch]);
    ui.lcdNumOutputRotZ->display(newdata[Roll]);

    if (_curve_config) {
        _curve_config->update();
    }
    if (Libraries->pProtocol)
    {
        const QString name = Libraries->pProtocol->getGameName();
        ui.game_name->setText(name);
    }
}

/** toggles Engine Controls Dialog **/
void FaceTrackNoIR::showTrackerSettings() {
	if (pTrackerDialog) {
		delete pTrackerDialog;
		pTrackerDialog = NULL;
	}

    DynamicLibrary* lib = dlopen_trackers.value(ui.iconcomboTrackerSource->currentIndex(), (DynamicLibrary*) NULL);

    if (lib) {
        pTrackerDialog = (ITrackerDialog*) lib->Dialog();
        if (pTrackerDialog) {
            auto foo = dynamic_cast<QWidget*>(pTrackerDialog);
            foo->setFixedSize(foo->size());
            if (Libraries && Libraries->pTracker)
                pTrackerDialog->registerTracker(Libraries->pTracker);
            pTrackerDialog->Initialize(this);
        }
    }
}

void FaceTrackNoIR::showSecondTrackerSettings() {
    if (pSecondTrackerDialog) {
        delete pSecondTrackerDialog;
        pSecondTrackerDialog = NULL;
    }

    DynamicLibrary* lib = dlopen_trackers.value(ui.cbxSecondTrackerSource->currentIndex() - 1, (DynamicLibrary*) NULL);

    if (lib) {
        pSecondTrackerDialog = (ITrackerDialog*) lib->Dialog();
        if (pSecondTrackerDialog) {
            auto foo = dynamic_cast<QWidget*>(pSecondTrackerDialog);
            foo->setFixedSize(foo->size());
            if (Libraries && Libraries->pSecondTracker)
                pSecondTrackerDialog->registerTracker(Libraries->pSecondTracker);
            pSecondTrackerDialog->Initialize(this);
        }
    }
}

void FaceTrackNoIR::showServerControls() {
    if (pProtocolDialog) {
        delete pProtocolDialog;
        pProtocolDialog = NULL;
    }

    DynamicLibrary* lib = dlopen_protocols.value(ui.iconcomboProtocol->currentIndex(), (DynamicLibrary*) NULL);

    if (lib && lib->Dialog) {
        pProtocolDialog = (IProtocolDialog*) lib->Dialog();
        if (pProtocolDialog) {
            auto foo = dynamic_cast<QWidget*>(pProtocolDialog);
            foo->setFixedSize(foo->size());
            pProtocolDialog->Initialize(this);
        }
    }
}

void FaceTrackNoIR::showFilterControls() {
    if (pFilterDialog) {
        delete pFilterDialog;
        pFilterDialog = NULL;
    }

    DynamicLibrary* lib = dlopen_filters.value(ui.iconcomboFilter->currentIndex(), (DynamicLibrary*) NULL);

    if (lib && lib->Dialog) {
        pFilterDialog = (IFilterDialog*) lib->Dialog();
        if (pFilterDialog) {
            auto foo = dynamic_cast<QWidget*>(pFilterDialog);
            foo->setFixedSize(foo->size());
            pFilterDialog->Initialize(this);
            if (Libraries && Libraries->pFilter)
                pFilterDialog->registerFilter(Libraries->pFilter);
        }
    }
}
void FaceTrackNoIR::showKeyboardShortcuts() {

	if (!_keyboard_shortcuts)
    {
        _keyboard_shortcuts = new KeyboardShortcutDialog( this, this );
    }

    _keyboard_shortcuts->show();
    _keyboard_shortcuts->raise();
}
void FaceTrackNoIR::showCurveConfiguration() {

	if (!_curve_config)
    {
        _curve_config = new CurveConfigurationDialog( this, this );
    }

	if (_curve_config) {
		_curve_config->show();
		_curve_config->raise();
	}
}

void FaceTrackNoIR::exit() {
	QCoreApplication::exit(0);
}

void FaceTrackNoIR::profileSelected(int index)
{
    if (looping)
        return;
	QSettings settings("opentrack");
	QString currentFile = settings.value ( "SettingsFile", QCoreApplication::applicationDirPath() + "/settings/default.ini" ).toString();
    QFileInfo pathInfo ( currentFile );

    settings.setValue ("SettingsFile", pathInfo.absolutePath() + "/" + iniFileList.value(index, ""));
	loadSettings();
}

#if !defined(_WIN32)
void FaceTrackNoIR::bind_keyboard_shortcut(QxtGlobalShortcut& key, const QString label, QSettings& iniFile)
{
    const int idx = iniFile.value("Key_index_" + label, 0).toInt();
    key.setShortcut(QKeySequence::fromString(""));
    key.setDisabled();
    QString seq(global_key_sequences.value(idx, ""));
    if (idx > 0)
    {
        if (!seq.isEmpty())
        {
            if (iniFile.value(QString("Shift_%1").arg(label), false).toBool())
                seq = "Shift+" + seq;
            if (iniFile.value(QString("Alt_%1").arg(label), false).toBool())
                seq = "Alt+" + seq;
            if (iniFile.value(QString("Ctrl_%1").arg(label), false).toBool())
                seq = "Ctrl+" + seq;
            key.setShortcut(QKeySequence::fromString(seq, QKeySequence::PortableText));
            key.setEnabled();
        } else {
	    key.setDisabled();
	}
    }
}
#else
static void bind_keyboard_shortcut(Key& key, const QString label, QSettings& iniFile)
{
    const int idx = iniFile.value("Key_index_" + label, 0).toInt();
    if (idx > 0)
    {
        key.keycode = 0;
        key.shift = key.alt = key.ctrl = 0;
        if (idx < global_windows_key_sequences.size())
            key.keycode = global_windows_key_sequences[idx];
        key.shift = iniFile.value(QString("Shift_%1").arg(label), false).toBool();
        key.alt = iniFile.value(QString("Alt_%1").arg(label), false).toBool();
        key.ctrl = iniFile.value(QString("Ctrl_%1").arg(label), false).toBool();
    }
}
#endif

void FaceTrackNoIR::bindKeyboardShortcuts()
{
    QSettings settings("opentrack");

    QString currentFile = settings.value ( "SettingsFile", QCoreApplication::applicationDirPath() + "/settings/default.ini" ).toString();
    QSettings iniFile( currentFile, QSettings::IniFormat );
    iniFile.beginGroup ( "KB_Shortcuts" );
    
#if !defined(_WIN32)
    bind_keyboard_shortcut(keyCenter, "Center", iniFile);
    bind_keyboard_shortcut(keyToggle, "Toggle", iniFile);
#else
    bind_keyboard_shortcut(keyCenter, "Center", iniFile);
    bind_keyboard_shortcut(keyToggle, "Toggle", iniFile);
#endif
    iniFile.endGroup ();

    if (tracker) /* running already */
    {
#if defined(_WIN32)
        if (keybindingWorker)
        {
            keybindingWorker->should_quit = true;
            keybindingWorker->wait();
            delete keybindingWorker;
            keybindingWorker = NULL;
        }
        keybindingWorker = new KeybindingWorker(*this, keyCenter, keyToggle);
        keybindingWorker->start();
#endif
    }
}

void FaceTrackNoIR::shortcutRecentered()
{
    QApplication::beep();

    qDebug() << "Center";
    if (tracker)
    {
        tracker->do_center = true;
    }
}

void FaceTrackNoIR::shortcutToggled()
{
    QApplication::beep();
    qDebug() << "Toggle";
    if (tracker)
    {
        tracker->enabled = !tracker->enabled;
    }
}
