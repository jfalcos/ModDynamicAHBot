#pragma once
// ProfessionMats.h – comprehensive reagent tables for ModDynamicAH
// Each table is an std::array<MatBracket, N> identical to the old in-cpp ones.
// Skill ranges follow WotLK trainer recipe break-points.

#include <cstdint>
#include <initializer_list>
#include <array>

struct MatBracket
{
    uint16 minSkill, maxSkill;
    std::initializer_list<uint32> items;
};

// -------------------------------- Cloth / Bandage (Tailoring + First Aid)
static const std::array<MatBracket, 7> TAILORING_CLOTH =
    {{
        {1, 75, {2589}},     // Linen Cloth
        {75, 125, {2592}},   // Wool Cloth
        {125, 175, {4306}},  // Silk Cloth
        {175, 225, {4338}},  // Mageweave Cloth
        {225, 300, {14047}}, // Runecloth
        {300, 350, {21877}}, // Netherweave
        {350, 450, {33470}}, // Frostweave
    }};

// -------------------------------- Herbs (Herbalism / Alchemy / Inscription)
static const std::array<MatBracket, 10> HERBS =
    {{
        {1, 70, {2447, 765}},                     // Silverleaf, Peacebloom
        {70, 115, {2449, 785, 2448}},             // Earthroot, Mageroyal, Briarthorn
        {115, 165, {2453, 3820, 2450}},           // Bruiseweed, Stranglekelp, Swiftthistle
        {165, 205, {3356, 3369, 3355}},           // Kingsblood, Grave Moss, Wild Steelbloom
        {205, 230, {3358, 3818, 3819}},           // Khadgar's Whisker, Fadeleaf, Dragon's Teeth
        {230, 270, {3821, 4625, 8836}},           // Goldthorn, Firebloom, Arthas' Tears
        {270, 300, {8831, 8839, 8845}},           // Purple Lotus, Blindweed, Ghost Mushroom
        {300, 325, {22785, 22786, 22787}},        // Felweed, Dreaming Glory, Ragveil
        {325, 350, {22789, 22790, 36901}},        // Terocone, Ancient Lichen, Goldclover
        {350, 450, {36903, 36904, 36907, 37921}}, // Adder's Tongue, Tiger Lily, Talandra's Rose, Deadnettle
    }};

// -------------------------------- Mining: Ore (for Mining, JC prospecting)
static const std::array<MatBracket, 9> MINING_ORE =
    {{
        {1, 65, {2770}},          // Copper Ore
        {65, 125, {2771}},        // Tin Ore
        {125, 175, {2772, 2775}}, // Iron, Silver
        {175, 230, {3858, 7911}}, // Mithril, Truesilver
        {230, 300, {10620}},      // Thorium
        {300, 325, {23424}},      // Fel Iron
        {325, 350, {23425}},      // Adamantite
        {350, 395, {36909}},      // Cobalt
        {395, 450, {36912}},      // Saronite
    }};

// -------------------------------- Smelt bars (Engineering helper)
static const std::array<MatBracket, 9> SMELTING_BARS =
    {{
        {1, 65, {2840}},            // Copper Bar
        {65, 125, {2841}},          // Bronze Bar
        {125, 150, {3575}},         // Iron Bar
        {150, 200, {3859}},         // Steel Bar
        {200, 250, {3860}},         // Mithril Bar
        {250, 300, {12359}},        // Thorium Bar
        {300, 325, {23445}},        // Fel Iron Bar
        {325, 350, {23446}},        // Adamantite Bar
        {350, 450, {36916, 36913}}, // Cobalt / Saronite Bars
    }};

// -------------------------------- Blacksmithing bars (trainable path)
static const std::array<MatBracket, 10> BS_BARS =
    {{
        {1, 75, {2840}},          // Copper
        {75, 125, {2841, 3576}},  // Bronze, Tin
        {125, 150, {3575}},       // Iron
        {150, 200, {3859, 3577}}, // Steel, Gold
        {200, 250, {3860}},       // Mithril
        {250, 300, {12359}},      // Thorium
        {300, 325, {23445}},      // Fel Iron
        {325, 350, {23446}},      // Adamantite
        {350, 420, {36916}},      // Cobalt
        {420, 450, {36913}},      // Saronite
    }};

// -------------------------------- Leathers (Skinning / Leatherworking)
static const std::array<MatBracket, 7> LEATHERS =
    {{
        {1, 75, {2318}},     // Light Leather
        {75, 125, {2319}},   // Medium Leather
        {125, 200, {4234}},  // Heavy Leather
        {200, 250, {4304}},  // Thick Leather
        {250, 300, {8170}},  // Rugged Leather
        {300, 350, {21887}}, // Knothide Leather
        {350, 450, {33568}}, // Borean Leather
    }};

// -------------------------------- Enchanting dusts (plus Essences if needed)
static const std::array<MatBracket, 7> ENCH_DUSTS =
    {{
        {1, 120, {10940}},   // Strange Dust
        {120, 180, {11083}}, // Soul Dust
        {180, 240, {11137}}, // Vision Dust
        {240, 300, {11176}}, // Dream Dust
        {300, 325, {16204}}, // Illusion Dust
        {325, 375, {22445}}, // Arcane Dust
        {375, 450, {34054}}, // Infinite Dust
    }};

// -------------------------------- Stones (Engineering bombs, etc.)
static const std::array<MatBracket, 5> MINING_STONE =
    {{
        {1, 65, {2835}},     // Rough Stone
        {65, 125, {2836}},   // Coarse Stone
        {125, 175, {2838}},  // Heavy Stone
        {175, 250, {7912}},  // Solid Stone
        {250, 450, {12365}}, // Dense Stone
    }};

// -------------------------------- Cooking meats (not vendor)
static const std::array<MatBracket, 8> COOKING_MEAT =
    {{
        {1, 60, {769, 2672}},       // Chunk of Boar / Stringy Wolf
        {60, 120, {3173, 3667}},    // Bear Meat / Tender Croc Meat
        {120, 180, {3730, 3731}},   // Big Bear / Lion Meat
        {180, 240, {3712, 12223}},  // Turtle Meat / Meaty Bat Wing
        {240, 300, {3174, 12037}},  // Spider Ichor / Mystery Meat
        {300, 325, {27668, 27669}}, // Lynx / Raptor Ribs
        {325, 350, {27682, 31670}}, // Talbuk Venison / Raptor Ribs (BC)
        {350, 450, {43013, 43009}}, // Chilled Meat / Shoveltusk Flank
    }};

// -------------------------------- Fishing / raw fish (Cooking & Fishing)
static const std::array<MatBracket, 8> FISHING_RAW =
    {{
        {1, 75, {6289, 6291}},      // Raw Longjaw / Raw Brilliant Smallfish
        {75, 150, {6308, 6362}},    // Raw Bristle Whisker / Raw Rockscale
        {150, 225, {6359, 6361}},   // Firefin Snapper / Raw Rainbow Fin
        {225, 300, {13754, 13758}}, // Raw Glossy Mightfish / Raw Redgill
        {300, 325, {27422, 27425}}, // Barbed Gill Trout / Spotted Feltail
        {325, 350, {27429, 27437}}, // Zangarian Sporefish / Icefin Bluefish
        {350, 400, {41809, 41802}}, // Glacial Salmon / Imperial Manta Ray
        {400, 450, {41808, 53063}}, // Bonescale Snapper / Mountain Trout
    }};

// -------------------------------- JC prospect gems by skill
static const std::array<MatBracket, 6> JEWELCRAFT_GEMS =
    {{
        {1, 180, {774, 818, 1210, 1206}},         // Malachite, Tigerseye, Shadowgem, Moss Agate
        {180, 230, {1705, 1529}},                 // Lesser Moonstone, Jade
        {230, 300, {7910, 7909, 3864}},           // Star Ruby, Aquamarine, Citrine
        {300, 325, {23112, 23107}},               // Golden Draenite, Shadow Draenite
        {325, 350, {23436, 23440}},               // Living Ruby, Dawnstone
        {350, 450, {36917, 36920, 36929, 36932}}, // Bloodstone, Chalcedony, Huge Citrine, Dark Jade
    }};

// ------------------------------------------------ Enchanting Essences
static const std::array<MatBracket, 7> ENCH_ESSENCE =
    {{
        {1, 70, {10938}},           // Lesser Magic Essence
        {70, 150, {10998}},         // Lesser Astral
        {150, 225, {11134, 11174}}, // Lesser Mystic / Nether
        {225, 300, {16202, 16203}}, // Lesser / Greater Eternal
        {300, 350, {22447}},        // Lesser Planar
        {350, 450, {34056, 34057}}, // Lesser / Greater Cosmic
    }};

// ------------------------------------------------ Enchanting Shards
static const std::array<MatBracket, 6> ENCH_SHARDS =
    {{
        {1, 150, {10978, 10998}},   // Small Glimmering, (dup ess placeholder)
        {150, 225, {11138, 11139}}, // Small/ Large Glowing
        {225, 285, {11174, 11175}}, // Lesser/Large Nether*
        {285, 350, {22448, 22449}}, // Small/Large Prismatic
        {350, 430, {34052, 34053}}, // Dream / Nightmare Shard
        {430, 450, {52718, 52721}}, // (Cata shards, weight very low)
    }};

// ------------------------------------------------ Elementals / Primals / Eternals
static const std::array<MatBracket, 4> ELEMENTALS =
    {{
        {300, 330, {22451, 22452, 22456}}, // Primal Air, Earth, Shadow
        {330, 375, {22457, 21884, 21885}}, // Primal Mana, Fire, Water
        {375, 425, {37701, 37702, 37703}}, // Crystallized Earth, Fire, Shadow
        {425, 450, {35622, 35623, 35627}}, // Eternal Water, Fire, Shadow
    }};

// ------------------------------------------------ Rare raws
static const std::array<MatBracket, 4> RARE_RAW =
    {{
        {250, 310, {12655}},               // Enchanted Thorium Bar (weight 0.2)
        {330, 375, {23571, 25707}},        // Primal Might, Fel Leather
        {375, 450, {33568, 43007, 45087}}, // Borean Leather, Northern Spices, Runed Orb
        {430, 450, {47556}},               // Crusader Orb
    }};
