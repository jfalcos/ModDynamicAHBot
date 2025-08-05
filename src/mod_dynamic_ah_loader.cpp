/*
 * Loader export required by AzerothCore.
 * Folder name: mod_dynamic_ah
 * The core will call: Addmod_dynamic_ahScripts()
 * We forward that to AddDynamicAhScripts() implemented in ModDynamicAH.cpp
 */

void AddDynamicAhScripts(); // implemented in ModDynamicAH.cpp

void Addmod_dynamic_ahScripts()
{
    AddDynamicAhScripts();
}
