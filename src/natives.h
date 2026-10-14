#pragma once

// RDR2 native hashes (64-bit) verified against a working RDR2 ASI plugin
// (Disquse/rdr3noclip) targeting the current game build, not the stale
// alloc8or nativedb which reports hashes for an older build. The
// ScriptHookRDR2 invoker matches natives by these 64-bit hashes.
namespace natives {

constexpr unsigned long long GET_PLAYER_INDEX            = 0x47E385B0D957C8D4ull;
constexpr unsigned long long GET_PLAYER_PED              = 0x275F255ED201B937ull;
constexpr unsigned long long PLAYER_PED_ID               = 0x096275889B8E0EE0ull;
constexpr unsigned long long GET_ENTITY_COORDS           = 0xA86D5F069399F44Dull;
constexpr unsigned long long SET_ENTITY_COORDS           = 0x06843DA7060A026Bull;
constexpr unsigned long long GET_ENTITY_HEADING          = 0xC230DD956E2F5507ull;
constexpr unsigned long long SET_ENTITY_HEADING          = 0xCF2B9C0645C4651Bull;
constexpr unsigned long long GET_ENTITY_MODEL            = 0xDA76A9F39210D365ull;
constexpr unsigned long long CREATE_PED                  = 0xD49F9B0955C367DEull;
constexpr unsigned long long REQUEST_MODEL               = 0xFA28FE3A6246FC30ull;
constexpr unsigned long long HAS_MODEL_LOADED            = 0x1283B8B89DD5D1B6ull;
constexpr unsigned long long DOES_ENTITY_EXIST           = 0xD42BD6EB2E0F1677ull;
constexpr unsigned long long IS_ENTITY_A_PED             = 0xCF8176912DDA4EA5ull;
constexpr unsigned long long SET_ENTITY_VISIBLE          = 0x1794B4FCC84D812Full;
constexpr unsigned long long IS_ENTITY_VISIBLE           = 0xFFC96ECB7FA404CAull;
constexpr unsigned long long DOES_ENTITY_HAVE_DRAWABLE   = 0x20487F0DA9AF164Aull;
constexpr unsigned long long SET_ENTITY_COLLISION        = 0xF66F820909453B8Cull;
constexpr unsigned long long SET_ENTITY_COMPLETELY_DISABLE_COLLISION = 0xE0580EC84813875Aull;
constexpr unsigned long long FREEZE_ENTITY_POSITION      = 0x7D9EFB7AD6B19754ull;
constexpr unsigned long long DELETE_ENTITY               = 0x4CD38C78BD19A497ull;
// DELETE_PED (0xCC0EF140F99365C5) принимает Ped* (указатель на handle,
// зануляет его) — НЕ значение; иначе пед не удаляется (как REMOVE_BLIP).
constexpr unsigned long long DELETE_PED                   = 0xCC0EF140F99365C5ull;
constexpr unsigned long long SET_ENTITY_AS_NO_LONGER_NEEDED = 0x4971D2F8162B9674ull;
constexpr unsigned long long CREATE_OBJECT               = 0x509D5878EB39E842ull;
constexpr unsigned long long SET_OBJECT_AS_NO_LONGER_NEEDED = 0x3AE22DEB5BA5A3E6ull;
constexpr unsigned long long SET_ENTITY_AS_MISSION_ENTITY = 0xDC19C288082E586Eull;
constexpr unsigned long long SET_PED_RANDOM_COMPONENT_VARIATION = 0xC8A9481A01E63C28ull;
constexpr unsigned long long SET_BLOCKING_OF_NON_TEMPORARY_EVENTS = 0x9F8AA94D6D97DBF4ull;
constexpr unsigned long long IS_MODEL_VALID              = 0x392C8D8E07B70EFCull;
constexpr unsigned long long TASK_STAND_STILL            = 0x919BE13EED931959ull;
constexpr unsigned long long PLACE_OBJECT_ON_GROUND_PROPERLY = 0x58A850EAEE20FAA3ull;
constexpr unsigned long long UI_BG_SET_TEXT_SCALE   = 0xA1253A3C870B6843ull;
constexpr unsigned long long UI_BG_SET_TEXT_COLOR   = 0x16FA5CE47F184F1Eull;
constexpr unsigned long long UI_BG_DISPLAY_TEXT     = 0x16794E044C9EFB58ull;
constexpr unsigned long long MISC_VAR_STRING        = 0xFA925AC00EB830B9ull;
constexpr unsigned long long MAP_BLIP_ADD_FOR_ENTITY = 0x23F74C2FDA6E7C61ull;
constexpr unsigned long long MAP_BLIP_ADD_FOR_COORDS = 0x554D9D53F696D002ull;
constexpr unsigned long long MAP_BLIP_ADD_MODIFIER  = 0x662D364ABF16DE2Full;
constexpr unsigned long long MAP_SET_BLIP_COORDS    = 0x4FF674F5E23D49CEull;
constexpr unsigned long long MAP_GET_BLIP_FROM_ENTITY = 0x6D2C41A8BD6D6FD0ull;
constexpr unsigned long long MAP_SET_BLIP_SPRITE    = 0x74F74D3207ED525Cull;
constexpr unsigned long long MAP_SET_BLIP_SCALE     = 0xD38744167B2FA257ull;
constexpr unsigned long long MAP_REMOVE_BLIP        = 0xF2C3C9DA47AAA54Aull;
// Yellow friendly-ally blip modifier (as in missions).
constexpr unsigned long long BLIP_MODIFIER_FRIENDLY   = 0x6414AA9Aull;
constexpr unsigned long long BLIP_MODIFIER_FRIENDLY_OBJECTIVE = 0xC586CF7Aull;
constexpr unsigned long long BLIP_MODIFIER_MP_PLAYER_ALLY = 0xB1AE1182ull;
// A valid companion blip style (makes the blip actually appear).
constexpr unsigned long long BLIP_STYLE_AMBIENT_COMPANION = 0xF4F30880ull;
constexpr unsigned long long BLIP_SPRITE_WHITE_DOT        = 0xB04092F8ull;
constexpr unsigned long long GET_PED_BONE_COORDS          = 0x17C07FC640E86B4Eull;
constexpr unsigned long long GET_SCREEN_COORD_FROM_WORLD_COORD = 0xCB50D7AFCC8B0EC6ull;
constexpr unsigned long long GET_DISTANCE_BETWEEN_COORDS  = 0x0BE7F4E3CDBAFB28ull;

// Проверка смерти педа (для метки-крестика и скрытия имени трупа).
constexpr unsigned long long IS_PED_DEAD_OR_DYING          = 0x3317DEDB88C95038ull;

// Определение/применение состояния движения (Фаза 4 — анимации).
constexpr unsigned long long IS_PED_RUNNING                = 0xC5286FFC176F28A2ull;
constexpr unsigned long long IS_PED_WALKING                = 0xDE4C184B2B9B071Aull;
constexpr unsigned long long IS_PED_ON_MOUNT               = 0x460BC76A0E10655Eull;
constexpr unsigned long long TASK_GO_STRAIGHT_TO_COORD     = 0xD76B57B44F1E6F8Bull;
// Высота земли в точке (для установки педа на землю, а не «в воздух»).
constexpr unsigned long long GET_GROUND_Z_FOR_3D_COORD      = 0x24FA4267BB8D2431ull;

// Полное состояние педа (Фаза 5 — полная синхронизация, стейт/флаги).
constexpr unsigned long long IS_PED_RAGDOLL                = 0x47E4E977581C5B55ull;
constexpr unsigned long long IS_PED_IN_COVER               = 0x60DFD0691A170B88ull;
constexpr unsigned long long IS_PED_SWIMMING               = 0x9DE327631295B4C2ull;
constexpr unsigned long long IS_PED_SWIMMING_UNDER_WATER   = 0xC024869A53992F34ull;
constexpr unsigned long long IS_PED_CLIMBING               = 0x53E8CB4F48BFE623ull;
constexpr unsigned long long IS_PED_JUMPING                = 0xCEDABC5900A0BF97ull;
constexpr unsigned long long IS_PED_FALLING                = 0xFB92A102F1C4DFA3ull;
constexpr unsigned long long IS_PED_SPRINTING              = 0x57E457CD2C0FC168ull;
constexpr unsigned long long IS_PED_WEAPON_READY_TO_SHOOT  = 0xB80CA294F2F26749ull;
constexpr unsigned long long GET_ENTITY_HEALTH             = 0x82368787EA73C0F7ull;
constexpr unsigned long long SET_CURRENT_PED_WEAPON        = 0xADF692B254977C0Cull;
constexpr unsigned long long SET_PED_TO_RAGDOLL            = 0xAE99FB955581844Aull;
// Хэш оружия в руке и текущая лошадь (для верховой езды).
constexpr unsigned long long GET_CURRENT_PED_WEAPON        = 0x3A87E44BB9A01D54ull;
constexpr unsigned long long GET_MOUNT                     = 0xE7E11B8DCBED1058ull;
// Верховая езда (получатель): посадить педа на лошадь / снять / перейти на лошадь.
constexpr unsigned long long SET_PED_ONTO_MOUNT            = 0x028F76B6E78246EBull;
constexpr unsigned long long TASK_MOUNT_ANIMAL             = 0x92DB0739813C5186ull;
constexpr unsigned long long TASK_DISMOUNT_ANIMAL          = 0x48E92D3DDE23C23Aull;
// Лошадь для показа верховой езды (a_c_horse_americanstandardbred_black).
constexpr unsigned long long HORSE_MODEL                   = 0xB57D0193ull;

// Мета-наряды (синхронизация внешнего вида игрока) — хэши сверены с
// rdr2-runtime-scripthook (совпадают с нашими проверенными нативами).
constexpr unsigned long long GET_PED_META_OUTFIT_HASH      = 0x30569F348D126A5Aull;
constexpr unsigned long long REQUEST_META_PED_OUTFIT       = 0x13154A76CE0CF9ABull;
constexpr unsigned long long HAS_META_PED_OUTFIT_LOADED    = 0x610438375E5D1801ull;
constexpr unsigned long long APPLY_PED_META_PED_OUTFIT     = 0x74F512E29CB717E2ull;
constexpr unsigned long long RELEASE_META_PED_OUTFIT_REQUEST = 0x4592B8B9B0EF5F48ull;

// Модификаторы метки: «скрыть при смерти» и «труп» (крестик на карте).
constexpr unsigned long long BLIP_MODIFIER_CULL_ON_DEATH   = 0xAA235D8Bull;
constexpr unsigned long long BLIP_ANIMAL_DEAD              = 0x4FE13DF7ull;

} // namespace natives