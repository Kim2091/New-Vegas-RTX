#pragma once
#include "api_common.hpp"

// Stealth fields: Stealth Boys, stealth armour, the Invisibility effect
// ([StealthField] in remix-comp.ini).
//
// The engine's look is a screen-space refraction, which never reaches the
// path-traced picture. With bUseRefractionShader=0 (game_ini.cpp) the engine
// doesn't even mark the actor: ChameleonEffect and InvisibilityEffect ask whether
// refraction is available (0x5B9B00) before the actor's SetRefraction (vtable
// +0x384, which sets SF_Refraction on its geometry through 0xB68770). Forcing
// that answer is no way out, because marked geometry gets another pass list
// (0xBC3E9B -> 0xB67BE0) and the actor could vanish.
//
// So the effect is read from the actors themselves: every few frames the player
// and the high-process actors are asked for Chameleon and Invisibility (actor
// values 49 and 48), and the scene roots of those above 0 are kept. A draw whose
// geometry hangs under such a root goes out as glass (api_skinned, api_statics;
// material::Desc::stealth). Render thread.
namespace comp::game::stealth_field
{
	// The geometry being drawn belongs to an actor under a stealth field.
	bool covers(uintptr_t geometry);

	// The glass such a surface is made of.
	void fill_glass(remixapi_MaterialInfoTranslucentEXT& tr);
}
