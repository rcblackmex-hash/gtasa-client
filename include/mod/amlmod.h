// =====================================================================
// AML SDK - minimal header for GTASAClient
//
// Solo declara lo que el cliente usa actualmente. El runtime real es
// AML v1.2.4 de RusJJ. Cuando necesitemos hooks reales en Hito 3.1
// (v0.23+), expandimos este header con el resto de la interfaz IAML
// (Hook, HookB, HookPLT, GetSym, etc.).
//
// Por ahora el mod usa la API VIEJA (OnModLoad(void*) + struct ModInfo
// global exportada via __GetModInfo). Esa API sigue soportada por AML.
// =====================================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Estructura que AML busca exportada como __GetModInfo()
struct ModInfo {
    const char* szGUID;
    const char* szName;
    const char* szAuthor;
    const char* szVersion;
};

// Callbacks que AML invoca
void OnModLoad(void* p);
void OnModUnload(void);

#ifdef __cplusplus
}
#endif
