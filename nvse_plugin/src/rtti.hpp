#pragma once
#include <cstdint>

// Gamebryo class identity from NiRTTI: vtable slot 2 (NiObject::GetRTTI) is
// `mov eax, imm32; ret` (or the same inside a frame), and the NiRTTI
// {name, parent} chain names the class and every base. Cached per vtable;
// main thread only.

namespace fnvr::rtti
{
	enum : uint32_t
	{
		kNode = 1u << 0,       // derives from NiNode
		kGeometry = 1u << 1,   // derives from NiGeometry
		kLight = 1u << 2,      // derives from NiLight
		kSourceTexture = 1u << 3,   // derives from NiSourceTexture
		kParticles = 1u << 4,  // derives from NiParticles (particle systems)
		kObjectNET = 1u << 5,  // derives from NiObjectNET (has a controller list)
		kController = 1u << 6, // derives from NiTimeController
		kKnown = 1u << 31,     // the chain resolved
	};

	// Class bits of the object at `obj` (0: unreadable or not an NiObject).
	uint32_t of(uintptr_t obj);
	uint32_t of_vtbl(uintptr_t vtbl);

	// The object's own NiRTTI class name ("?" when unreadable). Cached per
	// vtable; the pointer stays valid.
	const char* name(uintptr_t obj);

	// MSVC RTTI (any polymorphic engine class, such as a TESForm): the
	// offset of the base class subobject whose decorated name is
	// `type_name` (".?AVTESModel@@") inside the object, -1 when the object
	// has no such base or its RTTI does not read. Cached per vtable.
	int32_t base_offset(uintptr_t obj, const char* type_name);
}
