#include "TemplatesMenu.h"
#include <QPointer>
#include <exception>

#include "ConfigManager.h"
#include "Engine.h"
#include "Song.h"

#include "embed.h"
#include "GuiApplication.h"
#include "MainWindow.h"


namespace lmms::gui
{


TemplatesMenu::TemplatesMenu(QWidget *parent) :
	QMenu(tr("New from template"), parent)
{
	setIcon(embed::getIconPixmap("project_new"));

	connect( this, SIGNAL(aboutToShow()), SLOT(fillTemplatesMenu()));
	connect( this, SIGNAL(triggered(QAction*)),
		SLOT(createNewProjectFromTemplate(QAction*)));
}




void TemplatesMenu::createNewProjectFromTemplate(QAction * _action)
{
	const QPointer<TemplatesMenu> owner(this); const QPointer<Song> song(Engine::getSong());
	const QString templateFilePath = _action ? _action->data().toString() : QString{};
	if (templateFilePath.isEmpty()) { return; }
	if (getGUI()->mainWindow()->mayChangeProject(true) && owner && song && Engine::getSong() == song.data())
	{
		try { song->createNewProjectFromTemplate(templateFilePath); }
		catch (const std::exception& error) { if (song) { song->collectError(QString::fromUtf8(error.what())); } }
		catch (...) { if (song) { song->collectError("Native template creation failed"); } }
	}
}





void TemplatesMenu::fillTemplatesMenu()
{
	clear();

	addTemplatesFromDir(ConfigManager::inst()->userTemplateDir());
	addTemplatesFromDir(ConfigManager::inst()->factoryProjectsDir() + "templates");
}




void TemplatesMenu::addTemplatesFromDir( const QDir& dir ) {
	QFileInfoList templates = dir.entryInfoList( QStringList( "*.mpt" ),
		QDir::Files | QDir::Readable );

	if (!templates.empty() && !actions().isEmpty())
	{
		addSeparator();
	}

	auto projectFileIcon = embed::getIconPixmap( "project_file" );

	for(const QFileInfo& templateFile : templates)
	{
		auto action = addAction(projectFileIcon,
			templateFile.completeBaseName().replace("&", "&&"));
		action->setData(templateFile.absoluteFilePath());
#ifdef LMMS_BUILD_APPLE
		action->setIconVisibleInMenu(false); // QTBUG-44565 workaround
		action->setIconVisibleInMenu(true);
#endif
	}
}


} // namespace lmms::gui
