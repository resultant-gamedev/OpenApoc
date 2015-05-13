
#pragma once

#include "game/apocresources/apocresource.h"
#include "framework/font.h"
#include "forms/forms.h"
#include "vehiclefactory.h"

namespace OpenApoc {

class Framework;

class GameCore
{
	private:
		std::string language;

		std::map<std::string, std::string> languagetext;
		std::map<std::string, std::shared_ptr<BitmapFont>> fonts;
		std::map<std::string, Form*> forms;

		void ParseXMLDoc( std::string XMLFilename );
		void ParseGameXML( tinyxml2::XMLElement* Source );
		void ParseStringXML( tinyxml2::XMLElement* Source );
		void ParseFormXML( tinyxml2::XMLElement* Source );

		// void ParseAlienXML( tinyxml2::XMLElement* Source );
		// void ParseInventoryXML( tinyxml2::XMLElement* Source );
		// void ParseOrganisationXML( tinyxml2::XMLElement* Source );
		// void ParseUFOXML( tinyxml2::XMLElement* Source );
		// void ParseUFOpaediaXML( tinyxml2::XMLElement* Source );
		// void ParseVehicleXML( tinyxml2::XMLElement* Source );


		Framework &fw;



	public:
		bool Loaded;
		bool DebugModeEnabled;
		ApocCursor* MouseCursor;
		VehicleFactory vehicleFactory;

		GameCore(Framework &fw);
		void Load(std::string CoreXMLFilename, std::string Language);
		~GameCore();

		std::string GetString(std::string ID);
		Form* GetForm(std::string ID);
		std::shared_ptr<Image> GetImage(std::string ImageData);
		std::shared_ptr<BitmapFont> GetFont(std::string FontData);
		std::shared_ptr<Palette> GetPalette(std::string Path);
};

}; //namespace OpenApoc
