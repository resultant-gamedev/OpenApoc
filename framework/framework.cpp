
#include "framework/framework.h"
#include "game/boot.h"
#include "game/resources/gamecore.h"
#include "game/city/city.h"
#include "framework/renderer.h"
#include "framework/renderer_interface.h"
#include "framework/sound_interface.h"
#include "framework/sound.h"

#include <allegro5/allegro5.h>
#include <allegro5/allegro_font.h>
#include <allegro5/allegro_ttf.h>

namespace {

#ifndef DATA_DIRECTORY
#define DATA_DIRECTORY "./data"
#endif

static std::map<std::string, std::string> defaultConfig =
{
#ifdef PANDORA
	{"Visual.ScreenWidth", "800"},
	{"Visual.ScreenHeight", "480"},
	{"Visual.FullScreen", "true"},
#else
	{"Visual.ScreenWidth", "1600"},
	{"Visual.ScreenHeight", "900"},
	{"Visual.FullScreen", "false"},
#endif
	{"Language", "en_gb"},
	{"GameRules", "XCOMAPOC.XML"},
	{"Resource.LocalDataDir", "./data"},
	{"Resource.SystemDataDir", DATA_DIRECTORY},
	{"Resource.LocalCDPath", "./data/cd.iso"},
	{"Resource.SystemCDPath", DATA_DIRECTORY "/cd.iso"},
	{"Visual.RendererList", "GL_3_0;GL_2_1;allegro"},
	{"Audio.Backends", "allegro"},
};

std::map<std::string, std::unique_ptr<OpenApoc::RendererFactory>> *registeredRenderers = nullptr;
std::map<std::string, std::unique_ptr<OpenApoc::SoundBackendFactory>> *registeredSoundBackends = nullptr;

};

namespace OpenApoc {

void registerRenderer(RendererFactory* factory, std::string name)
{
	if (!registeredRenderers)
		registeredRenderers = new std::map<std::string, std::unique_ptr<OpenApoc::RendererFactory>>();
	registeredRenderers->emplace(name, std::unique_ptr<RendererFactory>(factory));
}

void registerSoundBackend(SoundBackendFactory *factory, std::string name)
{
	if (!registeredSoundBackends)
		registeredSoundBackends = new std::map<std::string, std::unique_ptr<OpenApoc::SoundBackendFactory>>();
	registeredSoundBackends->emplace(name, std::unique_ptr<SoundBackendFactory>(factory));
}


class JukeBoxImpl : public JukeBox
{
	Framework &fw;
	unsigned int position;
	std::vector<std::shared_ptr<MusicTrack>> trackList;
	JukeBox::PlayMode mode;
public:
	JukeBoxImpl(Framework &fw)
		:fw(fw), mode(JukeBox::PlayMode::Loop)
	{

	}
	virtual ~JukeBoxImpl()
	{
		this->stop();
	}

	virtual void play(std::vector<std::string> tracks, JukeBox::PlayMode mode)
	{
		this->trackList.clear();
		this->position = 0;
		this->mode = mode;
		for (auto &track : tracks)
		{
			auto musicTrack = fw.data->load_music(track);
			if (!musicTrack)
				LogError("Failed to load music track \"%s\" - skipping", track.c_str());
			else
				this->trackList.push_back(musicTrack);
		}
		this->progressTrack(this);
	}
	static void progressTrack(void *data)
	{
		JukeBoxImpl *jukebox = static_cast<JukeBoxImpl*>(data);
		if (jukebox->trackList.size() == 0)
		{
			LogWarning("Trying to play empty jukebox");
			return;
		}
		if (jukebox->position >= jukebox->trackList.size())
		{
			LogInfo("End of jukebox playlist");
			return;
		}
		jukebox->fw.soundBackend->playMusic(jukebox->trackList[jukebox->position], progressTrack, jukebox);
		jukebox->position++;
		if (jukebox->mode == JukeBox::PlayMode::Loop)
			jukebox->position = jukebox->position % jukebox->trackList.size();
	}
	virtual void stop()
	{
		fw.soundBackend->stopMusic();
	}
};

class FrameworkPrivate
{
	private:
		friend class Framework;
		bool quitProgram;


		ALLEGRO_DISPLAY_MODE screenMode;
		ALLEGRO_DISPLAY *screen;

		ALLEGRO_EVENT_QUEUE *eventAllegro;
		std::list<Event*> eventQueue;
		ALLEGRO_MUTEX *eventMutex;

		StageStack ProgramStages;
		std::shared_ptr<Surface> defaultSurface;
};



Framework::Framework(const std::string programName, const std::vector<std::string> cmdline)
	: p(new FrameworkPrivate), programName(programName)
{
	LogInfo("Starting framework");
	PHYSFS_init(programName.c_str());

	if( !al_init() )
	{
		LogError("Cannot init Allegro");
		p->quitProgram = true;
		return;
	}

	al_init_font_addon();
	if( !al_install_keyboard() || !al_install_mouse() || !al_init_ttf_addon())
	{
		LogError(" Cannot init Allegro plugins");
		p->quitProgram = true;
		return;
	}

	LogInfo("Loading config\n" );
	p->quitProgram = false;
	std::string settingsPath(PHYSFS_getPrefDir(PROGRAM_ORGANISATION, PROGRAM_NAME));
	settingsPath += "/settings.cfg";
	Settings.reset(new ConfigFile(settingsPath, defaultConfig));

	for (auto &option : cmdline)
	{
		auto splitString = Strings::Split(option, '=');
		if (splitString.size() != 2)
		{
			LogError("Failed to read command line option \"%s\" - ignoring", option.c_str());
			continue;
		}
		LogInfo("Setting option \"%s\" to \"%s\" from command line", splitString[0].c_str(), splitString[1].c_str());
		Settings->set(splitString[0], splitString[1]);

	}

	std::vector<std::string> resourcePaths;
	resourcePaths.push_back(Settings->getString("Resource.SystemCDPath"));
	resourcePaths.push_back(Settings->getString("Resource.LocalCDPath"));
	resourcePaths.push_back(Settings->getString("Resource.SystemDataDir"));
	resourcePaths.push_back(Settings->getString("Resource.LocalDataDir"));

	this->data.reset(new Data(*this, resourcePaths));

	auto testFile = this->data->load_file("MUSIC", "r");
	if (!testFile)
	{
		LogError("Failed to open \"music\" from the CD - likely the cd couldn't be loaded or paths are incorrect if using an extracted CD image");
	}

	p->eventAllegro = al_create_event_queue();
	p->eventMutex = al_create_mutex_recursive();

	srand( (unsigned int)al_get_time() );

	Display_Initialise();
	Audio_Initialise();

	al_register_event_source( p->eventAllegro, al_get_display_event_source( p->screen ) );
	al_register_event_source( p->eventAllegro, al_get_keyboard_event_source() );
	al_register_event_source( p->eventAllegro, al_get_mouse_event_source() );

}

Framework::~Framework()
{
	LogInfo("Destroying framework");
	//Kill gamecore and program stages first, so any resources are cleaned before
	//allegro is de-inited
	state.clear();
	gamecore.reset();
	p->ProgramStages.Clear();
	LogInfo("Saving config");
	SaveSettings();

	LogInfo("Shutdown");
	Display_Shutdown();
	Audio_Shutdown();
	al_destroy_event_queue( p->eventAllegro );
	al_destroy_mutex( p->eventMutex );

	LogInfo("Allegro shutdown");
	al_shutdown_ttf_addon();
	al_uninstall_mouse();
	al_uninstall_keyboard();
	al_shutdown_font_addon();

	al_uninstall_system();
	PHYSFS_deinit();
}

void Framework::Run()
{
	LogInfo("Program loop started");

	p->ProgramStages.Push( std::make_shared<BootUp>(*this) );

	this->renderer->setPalette(this->data->load_palette("xcom3/ufodata/PAL_01.DAT"));


	while( !p->quitProgram )
	{
		RendererSurfaceBinding b(*this->renderer, p->defaultSurface);
		this->renderer->clear();
		ProcessEvents();

		StageCmd cmd;
		if( p->ProgramStages.IsEmpty() )
		{
			break;
		}
		p->ProgramStages.Current()->Update(&cmd);
		switch (cmd.cmd)
		{
			case StageCmd::Command::CONTINUE:
				break;
			case StageCmd::Command::REPLACE:
				p->ProgramStages.Pop();
				p->ProgramStages.Push(cmd.nextStage);
				break;
			case StageCmd::Command::PUSH:
				p->ProgramStages.Push(cmd.nextStage);
				break;
			case StageCmd::Command::POP:
				p->ProgramStages.Pop();
				break;
			case StageCmd::Command::QUIT:
				p->quitProgram = true;
				p->ProgramStages.Clear();
				break;

		}
		if( !p->ProgramStages.IsEmpty() )
		{
			p->ProgramStages.Current()->Render();
			al_flip_display();
		}
	}
}

void Framework::ProcessEvents()
{
	LogInfo("Processing events");

	if( p->ProgramStages.IsEmpty() )
	{
		p->quitProgram = true;
		return;
	}

	// Convert Allegro events before we process
	// TODO: Consider threading the translation
	TranslateAllegroEvents();

	al_lock_mutex( p->eventMutex );

	while( p->eventQueue.size() > 0 && !p->ProgramStages.IsEmpty() )
	{
		Event* e;
		e = p->eventQueue.front();
		p->eventQueue.pop_front();
		switch( e->Type )
		{
			case EVENT_WINDOW_CLOSED:
				delete e;
				al_unlock_mutex( p->eventMutex );
				ShutdownFramework();
				return;
				break;
			default:
				p->ProgramStages.Current()->EventOccurred( e );
				break;
		}
		delete e;
	}

	al_unlock_mutex( p->eventMutex );
}

void Framework::PushEvent( Event* e )
{
	al_lock_mutex( p->eventMutex );
	p->eventQueue.push_back( e );
	al_unlock_mutex( p->eventMutex );
}

void Framework::TranslateAllegroEvents()
{
	ALLEGRO_EVENT e;
	Event* fwE;

	while( al_get_next_event( p->eventAllegro, &e ) )
	{
		switch( e.type )
		{
			case ALLEGRO_EVENT_DISPLAY_CLOSE:
				fwE = new Event();
				fwE->Type = EVENT_WINDOW_CLOSED;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_JOYSTICK_CONFIGURATION:
				al_reconfigure_joysticks();
				break;
			case ALLEGRO_EVENT_TIMER:
				fwE = new Event();
				fwE->Type = EVENT_TIMER_TICK;
				fwE->Data.Timer.TimerObject = (void*)e.timer.source;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_KEY_DOWN:
				fwE = new Event();
				fwE->Type = EVENT_KEY_DOWN;
				fwE->Data.Keyboard.KeyCode = e.keyboard.keycode;
				fwE->Data.Keyboard.UniChar = e.keyboard.unichar;
				fwE->Data.Keyboard.Modifiers = e.keyboard.modifiers;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_KEY_UP:
				fwE = new Event();
				fwE->Type = EVENT_KEY_UP;
				fwE->Data.Keyboard.KeyCode = e.keyboard.keycode;
				fwE->Data.Keyboard.UniChar = e.keyboard.unichar;
				fwE->Data.Keyboard.Modifiers = e.keyboard.modifiers;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_KEY_CHAR:
				fwE = new Event();
				fwE->Type = EVENT_KEY_PRESS;
				fwE->Data.Keyboard.KeyCode = e.keyboard.keycode;
				fwE->Data.Keyboard.UniChar = e.keyboard.unichar;
				fwE->Data.Keyboard.Modifiers = e.keyboard.modifiers;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_MOUSE_AXES:
				fwE = new Event();
				fwE->Type = EVENT_MOUSE_MOVE;
				fwE->Data.Mouse.X = e.mouse.x;
				fwE->Data.Mouse.Y = e.mouse.y;
				fwE->Data.Mouse.DeltaX = e.mouse.dx;
				fwE->Data.Mouse.DeltaY = e.mouse.dy;
				fwE->Data.Mouse.WheelVertical = e.mouse.dz;
				fwE->Data.Mouse.WheelHorizontal = e.mouse.dw;
				fwE->Data.Mouse.Button = e.mouse.button;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_MOUSE_BUTTON_DOWN:
				fwE = new Event();
				fwE->Type = EVENT_MOUSE_DOWN;
				fwE->Data.Mouse.X = e.mouse.x;
				fwE->Data.Mouse.Y = e.mouse.y;
				fwE->Data.Mouse.DeltaX = e.mouse.dx;
				fwE->Data.Mouse.DeltaY = e.mouse.dy;
				fwE->Data.Mouse.WheelVertical = e.mouse.dz;
				fwE->Data.Mouse.WheelHorizontal = e.mouse.dw;
				fwE->Data.Mouse.Button = e.mouse.button;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_MOUSE_BUTTON_UP:
				fwE = new Event();
				fwE->Type = EVENT_MOUSE_UP;
				fwE->Data.Mouse.X = e.mouse.x;
				fwE->Data.Mouse.Y = e.mouse.y;
				fwE->Data.Mouse.DeltaX = e.mouse.dx;
				fwE->Data.Mouse.DeltaY = e.mouse.dy;
				fwE->Data.Mouse.WheelVertical = e.mouse.dz;
				fwE->Data.Mouse.WheelHorizontal = e.mouse.dw;
				fwE->Data.Mouse.Button = e.mouse.button;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_DISPLAY_RESIZE:
				fwE = new Event();
				fwE->Type = EVENT_WINDOW_RESIZE;
				fwE->Data.Display.X = 0;
				fwE->Data.Display.Y = 0;
				fwE->Data.Display.Width = al_get_display_width( p->screen );
				fwE->Data.Display.Height = al_get_display_height( p->screen );
				fwE->Data.Display.Active = true;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_DISPLAY_SWITCH_IN:
				fwE = new Event();
				fwE->Type = EVENT_WINDOW_ACTIVATE;
				fwE->Data.Display.X = 0;
				fwE->Data.Display.Y = 0;
				fwE->Data.Display.Width = al_get_display_width( p->screen );
				fwE->Data.Display.Height = al_get_display_height( p->screen );
				fwE->Data.Display.Active = true;
				PushEvent( fwE );
				break;
			case ALLEGRO_EVENT_DISPLAY_SWITCH_OUT:
				fwE = new Event();
				fwE->Type = EVENT_WINDOW_DEACTIVATE;
				fwE->Data.Display.X = 0;
				fwE->Data.Display.Y = 0;
				fwE->Data.Display.Width = al_get_display_width( p->screen );
				fwE->Data.Display.Height = al_get_display_height( p->screen );
				fwE->Data.Display.Active = false;
				PushEvent( fwE );
				break;
			default:
				fwE = new Event();
				fwE->Type = EVENT_UNDEFINED;
				PushEvent( fwE );
				break;
		}
	}
}

void Framework::ShutdownFramework()
{
	LogInfo("Shutdown framework");
	p->ProgramStages.Clear();
	p->quitProgram = true;
}

void Framework::SaveSettings()
{
	// Just to keep the filename consistant
	std::string settingsPath(PHYSFS_getPrefDir(PROGRAM_ORGANISATION, PROGRAM_NAME));
	settingsPath += "/settings.cfg";
	Settings->save( settingsPath );
}

void Framework::Display_Initialise()
{
	LogInfo("Init display");
	int display_flags = ALLEGRO_OPENGL;
#ifdef ALLEGRO_OPENGL_CORE
	display_flags |= ALLEGRO_OPENGL_CORE;
#endif

#if ALLEGRO_VERSION > 5 || (ALLEGRO_VERSION == 5 && ALLEGRO_SUB_VERSION >= 1)
	display_flags |= ALLEGRO_OPENGL_3_0 | ALLEGRO_PROGRAMMABLE_PIPELINE;
#endif

	int scrW = Settings->getInt("Visual.ScreenWidth");
	int scrH = Settings->getInt("Visual.ScreenHeight");
	bool scrFS = Settings->getBool("Visual.FullScreen");

	if( scrFS )
	{
		display_flags |= ALLEGRO_FULLSCREEN;
	}

	al_set_new_display_flags(display_flags);

	p->screen = al_create_display( scrW, scrH );

	if (!p->screen)
	{
		LogError("Failed to create screen");;
		exit(1);
	}

	al_set_blender( ALLEGRO_ADD, ALLEGRO_ALPHA, ALLEGRO_INVERSE_ALPHA );

	al_hide_mouse_cursor( p->screen );

	for (auto &rendererName : Strings::Split(Settings->getString("Visual.RendererList"), ';'))
	{
		auto rendererFactory = registeredRenderers->find(rendererName);
		if (rendererFactory == registeredRenderers->end())
		{
			LogInfo("Renderer \"%s\" not in supported list", rendererName.c_str());
			continue;
		}
		Renderer *r = rendererFactory->second->create();
		if (!r)
		{
			LogInfo("Renderer \"%s\" failed to init", rendererName.c_str());
			continue;
		}
		this->renderer.reset(r);
		LogInfo("Using renderer: %s", this->renderer->getName().c_str());
		break;
	}
	if (!this->renderer)
	{
		LogError("No functional renderer found");
		abort();
	}
	this->p->defaultSurface = this->renderer->getDefaultSurface();


}

void Framework::Display_Shutdown()
{
	LogInfo("Shutdown Display");
	p->defaultSurface.reset();
	renderer.reset();

	al_unregister_event_source( p->eventAllegro, al_get_display_event_source( p->screen ) );
	al_destroy_display( p->screen );
}

int Framework::Display_GetWidth()
{
	return al_get_display_width( p->screen );
}

int Framework::Display_GetHeight()
{
	return al_get_display_height( p->screen );
}

void Framework::Display_SetTitle( std::string* NewTitle )
{
	std::wstring widestr = std::wstring(NewTitle->begin(), NewTitle->end());
	al_set_app_name( (char*)widestr.c_str() );
	al_set_window_title( p->screen, (char*)widestr.c_str() );
}

void Framework::Display_SetTitle( std::string NewTitle )
{
	std::wstring widestr = std::wstring(NewTitle.begin(), NewTitle.end());
	al_set_app_name( (char*)widestr.c_str() );
	al_set_window_title( p->screen, (char*)widestr.c_str() );
}

void Framework::Audio_Initialise()
{
	LogInfo("Initialise Audio");

	for (auto &soundBackendName : Strings::Split(Settings->getString("Audio.Backends"), ';'))
	{
		auto backendFactory = registeredSoundBackends->find(soundBackendName);
		if (backendFactory == registeredSoundBackends->end())
		{
			LogInfo("Sound backend %s not in supported list", soundBackendName.c_str());
			continue;
		}
		SoundBackend *backend = backendFactory->second->create();
		if (!backend)
		{
			LogInfo("Sound backend %s failed to init", soundBackendName.c_str());
			continue;
		}
		this->soundBackend.reset(backend);
		LogInfo("Using sound backend %s", soundBackendName.c_str());
		break;
	}
	if (!this->soundBackend)
	{
		LogError("No functional sound backend found");
		abort();
	}
	this->jukebox.reset(new JukeBoxImpl(*this));
}

void Framework::Audio_Shutdown()
{
	LogInfo("Shutdown Audio");
	this->jukebox.reset();
	this->soundBackend.reset();
}

}; //namespace OpenApoc
