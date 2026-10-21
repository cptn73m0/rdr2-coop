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
// Снять столкновения МЕЖДУ двумя сущностями (thisFrameOnly=true — вызывать
// каждый кадр). Нужно для лошади-призрака: мир/земля остаются коллизионными,
// а с локальным игроком и его лошадью она не толкается.
constexpr unsigned long long SET_ENTITY_NO_COLLISION_ENTITY = 0xE037BF068223C38Dull;
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
// Игрок реально целится (свободный прицел, не «оружие наготове»).
constexpr unsigned long long IS_PLAYER_FREE_AIMING          = 0x936F967D4BE1CE9Dull;
constexpr unsigned long long GET_ENTITY_HEALTH             = 0x82368787EA73C0F7ull;
constexpr unsigned long long SET_CURRENT_PED_WEAPON        = 0xADF692B254977C0Cull;
constexpr unsigned long long SET_PED_TO_RAGDOLL            = 0xAE99FB955581844Aull;
// Анимации-задачи для педа-призрака (прыжок/лазание) — хэши из
// rdr2-runtime-scripthook (TASK_JUMP / TASK_CLIMB).
constexpr unsigned long long TASK_JUMP                     = 0x0AE4086104E067B1ull;
constexpr unsigned long long TASK_CLIMB                    = 0x89D9FCC2435112F1ull;
// Хэш оружия в руке и текущая лошадь (для верховой езды).
constexpr unsigned long long GET_CURRENT_PED_WEAPON        = 0x3A87E44BB9A01D54ull;
// Оружие педа-призрака: выдать / снять / прицелиться / выстрелить.
// Хэши и порядок аргументов — из rdr2-runtime-scripthook (Не сравнивать
// с GTA: у GIVE_WEAPON_TO_PED здесь 13 параметров).
constexpr unsigned long long GIVE_WEAPON_TO_PED            = 0x5E3BDDBCB83F3D84ull;
constexpr unsigned long long REMOVE_WEAPON_FROM_PED        = 0x4899CB088EDF59B8ull;
// Патроны в магазине текущего оружия — надёжный детект КАЖДОГО выстрела
// (в т.ч. повторных попаданий в одну точку, где impact coord не меняется).
constexpr unsigned long long GET_AMMO_IN_CLIP              = 0x2E1202248937775Cull;
constexpr unsigned long long TASK_AIM_GUN_AT_COORD         = 0x6671F3EEC681BDA1ull;
constexpr unsigned long long TASK_SHOOT_AT_COORD           = 0x46A6CC01E0826106ull;
constexpr unsigned long long TASK_RELOAD_WEAPON            = 0x62D2916F56B9CD2Dull;
// Точка прицела/попадания (для точной стрельбы педа-призрака).
constexpr unsigned long long GET_ENTITY_PLAYER_IS_FREE_AIMING_AT = 0xA6817C110B830EADull;
constexpr unsigned long long GET_GAMEPLAY_CAM_COORD = 0x595320200B98596Eull;
constexpr unsigned long long GET_GAMEPLAY_CAM_ROT   = 0x0252D2B5582957A6ull;
constexpr unsigned long long GET_PED_LAST_WEAPON_IMPACT_COORD    = 0x6C4D0409BA1A2BC2ull;
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

// Время (игровые часы) и погода — синхронизация мира от хоста.
constexpr unsigned long long GET_CLOCK_HOURS               = 0xC82CF208C2B19199ull;
constexpr unsigned long long GET_CLOCK_MINUTES             = 0x4E162231B823DBBFull;
constexpr unsigned long long GET_CLOCK_SECONDS             = 0xB6101ABE62B5F080ull;
constexpr unsigned long long SET_CLOCK_TIME                = 0x3A52C59FFB2DEED8ull;
constexpr unsigned long long GET_CURR_WEATHER_STATE        = 0x0AC679B2342F14F2ull;
constexpr unsigned long long SET_WEATHER_TYPE              = 0x59174F1AFE095B5Aull;

// Компоненты одежды (наряд) педа — синхронизация внешнего вида. Хэши из
// rdr2-runtime-scripthook: перечислить одетые компоненты и применить их.
constexpr unsigned long long GET_NUM_COMPONENTS_IN_PED     = 0x90403E8107B60E81ull;
constexpr unsigned long long GET_PED_COMPONENT_CATEGORY_BY_INDEX = 0xCCB97B51893C662Full;
constexpr unsigned long long EQUIP_META_PED_OUTFIT         = 0x1902C4CFCC5BE57Cull;
constexpr unsigned long long UPDATE_PED_VARIATION          = 0xCC8CA3E88256E58Full;
constexpr unsigned long long RESET_PED_COMPONENTS          = 0x0BFA1BD465CDFEFDull;
// ОДЕЖДА: чтение и применение надетых shop-предметов (правильный механизм).
constexpr unsigned long long GET_NUM_COMPONENT_CATEGORIES_IN_PED = 0xA622E66EEE92A08Dull;
constexpr unsigned long long GET_SHOP_ITEM_COMPONENT_AT_INDEX    = 0x77BA37622E22023Bull;
constexpr unsigned long long APPLY_SHOP_ITEM_TO_PED              = 0xD3A7B003ED343FD9ull;
constexpr unsigned long long _REMOVE_SHOP_ITEM_FROM_PED          = 0x0D7FFA1B2F69ED82ull;
constexpr unsigned long long _GET_SHOP_ITEM_COMPONENT_CATEGORY   = 0x5FF9A878C3D115B8ull;
constexpr unsigned long long REMOVE_SHOP_ITEM_FROM_PED_BY_CATEGORY = 0xDF631E4BCE1B1FC4ull;
constexpr unsigned long long _GET_META_PED_TYPE                  = 0xEC9A1261BF0CE510ull;
constexpr unsigned long long _SET_ACTIVE_META_PED_COMPONENTS_UPDATED = 0xAAB86462966168CEull;
constexpr unsigned long long _UPDATE_SHOP_ITEM_WEARABLE_STATE     = 0x66B957AAC2EAAEABull;
constexpr unsigned long long _GET_SHOP_ITEM_NUM_WEARABLE_STATES   = 0xFFCC2DB2D9953401ull;
constexpr unsigned long long _GET_SHOP_ITEM_WEARABLE_STATE_BY_INDEX = 0x6243635AF2F1B826ull;

// Реплей стрельбы педа-призрака (серия выстрелов без обрыва анимации).
constexpr unsigned long long TASK_SHOOT_AT_ENTITY             = 0x08DA95E8298AE772ull;
constexpr unsigned long long SET_PED_SHOOT_RATE               = 0x614DA022990752DCull;
constexpr unsigned long long CLEAR_PED_TASKS                  = 0xE1EF3C1216AFF2CDull;

// Бесконечные патроны у педа-призрака: иначе после ~60 выстрелов оружие
// пустеет и TASK_SHOOT_AT_COORD молча ничего не делает.
constexpr unsigned long long SET_PED_INFINITE_AMMO           = 0x3EDCB0505123623Bull;
constexpr unsigned long long _SET_PED_INFINITE_AMMO_CLIP     = 0xFBAA1E06B6BCA741ull;

// ДЕЙСТВИЯ: детект кратковременных/контекстных анимаций (стрельба,
// перезарядка, сидение, сценарий, прицеливание, пьян).
constexpr unsigned long long IS_PED_SHOOTING             = 0x34616828CD07F1A1ull;
constexpr unsigned long long IS_PED_RELOADING            = 0x24B100C68C645951ull;

// Анимации оружия педа-призрака: доставание/убирание/перезарядка.
constexpr unsigned long long _HIDE_PED_WEAPONS               = 0xFCCC886EDE3C63ECull;
constexpr unsigned long long _HOLSTER_PED_WEAPONS            = 0x94A3C1B804D291ECull;
constexpr unsigned long long _SET_WEAPON_HOLSTERED           = 0x4820A6939D7CEF28ull;
constexpr unsigned long long _IS_WEAPON_HOLSTER_STATE_CHANGING = 0x2387D6E9C6B478AAull;
constexpr unsigned long long _IS_PED_CURRENT_WEAPON_HOLSTERED  = 0xBDD9C235D8D1052Eull;
constexpr unsigned long long _GET_PED_CURRENT_HELD_WEAPON      = 0x8425C5F057012DABull;
constexpr unsigned long long _MAKE_PED_RELOAD                = 0x79E1E511FF7EFB13ull;
constexpr unsigned long long SET_AMMO_IN_CLIP                = 0xDCD2A934D65CB497ull;
constexpr unsigned long long _SET_FORCE_CURRENT_WEAPON_INTO_COCKED_STATE = 0x5230D3F6EE56CFE6ull;
constexpr unsigned long long WEAPON_UNARMED = 0xA2719263ull;
constexpr unsigned long long IS_PED_SITTING              = 0x84D0BF2B21862059ull;
constexpr unsigned long long IS_PED_USING_ANY_SCENARIO   = 0x57AB4A3080F85143ull;
constexpr unsigned long long IS_PED_AIMING_FROM_COVER    = 0x3998B1276A3300E5ull;
constexpr unsigned long long IS_PED_DRUNK                = 0x50F124E6EF188B22ull;
// Скорость сущности (для надёжного детекта движения, когда IS_PED_WALKING
// ложно при медленной ходьбе).
constexpr unsigned long long GET_ENTITY_SPEED             = 0xFB6BA510A533DF81ull;
// Бессмертие педа-призрака: чтобы он не умирал от коллизий/бандитов,
// а умирал только вместе с оригиналом (сервер шлёт Died).
constexpr unsigned long long SET_ENTITY_INVINCIBLE         = 0xA5C38736C426FCB8ull;
constexpr unsigned long long SET_ENTITY_HEALTH             = 0xAC2767ED8BDFAB15ull;
// Возрождение педа (когда оригинал ожил после смерти).
constexpr unsigned long long RESURRECT_PED                 = 0x71BC8E838B9C6035ull;
// Выносливость педа: чтобы пед-призрак не «отдувался» (не играл анимацию
// усталости) после бега. Множитель истощения 0 = не устаёт.
constexpr unsigned long long RESTORE_PED_STAMINA           = 0x675680D089BFA21Full;
constexpr unsigned long long SET_STAMINA_DEPLETION_MULTIPLIER = 0xEF5A3D2285D8924Bull;
constexpr unsigned long long GET_PED_MAX_STAMINA           = 0xCB42AFE2B613EE55ull;
// Ограничение скорости анимации педа (для шага/бега — чтобы пед-призрак
// шёл шагом, а не бежал, при движении задачей TASK_GO_*).
constexpr unsigned long long SET_PED_MAX_MOVE_BLEND_RATIO   = 0x433083750C5E064Aull;
// Плавное анимированное движение педа к координатам (walk/run по скорости).
constexpr unsigned long long TASK_FOLLOW_NAV_MESH_TO_COORD  = 0x15D3A79D4E44B913ull;

// Spike 1 (модель A): признак mission entity — приоритет репликации мира
// и фильтр для прокси-сущностей на госте.
constexpr unsigned long long IS_ENTITY_A_MISSION_ENTITY     = 0x138190F64DB4BBD1ull;

// P1 (модель A): подавление локальной популяции у гостя, чтобы мир хоста
// не дублировался собственными ambient-NPC гостем.
constexpr unsigned long long _SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME  = 0xBA0980B5C0A11924ull;
constexpr unsigned long long _SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME    = 0xAB0D553FE20A6E25ull;
constexpr unsigned long long _SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME = 0xC0258742B034DFAFull;
constexpr unsigned long long SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME         = 0x606374EBFC27B133ull;
constexpr unsigned long long SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME  = 0x1F91D44490E1EA0Cull;

// P2 (Action v2): наблюдаемые действия/сценарии. Клип/фазу AnimGraph
// публичный SDK не отдаёт; читаем сценарий/укрытие/переноску/и т.п.
constexpr unsigned long long _GET_ACTIVE_DYNAMIC_SCENARIO      = 0x569F1E1237508DEBull;
constexpr unsigned long long _GET_ACTIVE_DYNAMIC_SCENARIO_2    = 0xC22AA08A8ADB87D4ull;
constexpr unsigned long long IS_PED_CARRYING_SOMETHING         = 0xA911EE21EDF69DAFull;
constexpr unsigned long long _GET_FIRST_ENTITY_PED_IS_CARRYING = 0xD806CD2A4F2C2996ull;
constexpr unsigned long long _IS_PED_DRAGGING                  = 0x226CF9B159E38F42ull;
constexpr unsigned long long IS_PED_HOGTIED                    = 0x3AA24CCC0D451379ull;
constexpr unsigned long long GET_LOOTING_PICKUP_TARGET_ENTITY  = 0x14169FA823679E41ull;
constexpr unsigned long long _GET_TASK_FISHING                 = 0xF3735ACD11ACD500ull;
constexpr unsigned long long TASK_START_SCENARIO_IN_PLACE_HASH = 0x524B54361229154Full;

// P2: укрытие и действия с целью (переноска/лутание).
constexpr unsigned long long TASK_ENTER_COVER                 = 0x4972A022AE6DAFA1ull;
constexpr unsigned long long TASK_EXIT_COVER                  = 0x2BC4A6D92D140112ull;
constexpr unsigned long long TASK_PICKUP_CARRIABLE_ENTITY     = 0x502EC17B1BED4BFAull;
constexpr unsigned long long TASK_LOOT_ENTITY                 = 0x48FAE038401A2888ull;
constexpr unsigned long long TASK_HOGTIE_TARGET_PED           = 0x27829AFD3E03AC1Aull;

// P1: транспорт в WorldSnapshot.
constexpr unsigned long long CREATE_VEHICLE                   = 0xAF35D0D2583051B0ull;
constexpr unsigned long long DELETE_VEHICLE                   = 0xE20A909D8C4A70F8ull;
constexpr unsigned long long GET_VEHICLE_PED_IS_USING         = 0x6094AD011A2EA87Dull;
constexpr unsigned long long IS_VEHICLE_WRECKED               = 0xDDBEA5506C848227ull;
constexpr unsigned long long IS_THIS_MODEL_A_TRAIN            = 0xFC08C8F8C1EDF174ull;
constexpr unsigned long long _GET_TRAIN_CARRIAGE_TRAILER_NUMBER = 0x60B7D1DCC312697Dull;
constexpr unsigned long long GET_TRAIN_CARRIAGE               = 0xD0FB093A4CDB932Cull;
constexpr unsigned long long _GET_TRAIN_CAR                   = 0x671A07C9A1CD50A5ull;
constexpr unsigned long long _GET_TRAIN_DIRECTION             = 0x3C9628A811CBD724ull;
// R&D: привязка вагонов-прокси к локомотиву на госте.
constexpr unsigned long long ATTACH_ENTITY_TO_ENTITY          = 0x6B9BBD38AB0796DFull;
constexpr unsigned long long DETACH_ENTITY                    = 0x64CDE9D6BF8ECAD3ull;
constexpr unsigned long long IS_ENTITY_ATTACHED_TO_ENTITY     = 0x154A3C529497053Eull;
constexpr unsigned long long DELETE_OBJECT                    = 0x931914268722C263ull;
constexpr unsigned long long GET_VEHICLE_PED_IS_IN            = 0x9A9112A0FE9A4713ull;
constexpr unsigned long long GET_PED_IN_VEHICLE_SEAT          = 0xBB40DD2270B65366ull;
constexpr unsigned long long IS_PED_IN_VEHICLE                = 0xA3EE4A07279BB9DBull;
constexpr unsigned long long SET_PED_INTO_VEHICLE             = 0xF75B0D629E1C063Dull;

// P3: MissionData (только чтение/запись зачёта; никаких копий сейва).
constexpr unsigned long long MISSIONDATA_IS_VALID             = 0xE54DC27571D5EDC5ull;
constexpr unsigned long long MISSIONDATA_WAS_COMPLETED        = 0xE54DC27571D5EDC4ull;
constexpr unsigned long long MISSIONDATA_IS_REQUIRED_STORY_MISSION = 0xE824CE7D13FCB35Eull;
constexpr unsigned long long MISSIONDATA_GET_RATING           = 0x57E798B54C45EE1Aull;
constexpr unsigned long long _MISSIONDATA_SET_MISSION_RATING  = 0xE824CE7D13FCB300ull;
constexpr unsigned long long _UILOG_MARK_MISSION_COMPLETED    = 0xDE31D66D1E54C471ull;
constexpr unsigned long long GET_MISSION_FLAG                 = 0xB15CD1CF58771DE1ull;
constexpr unsigned long long GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH =
    0x2C83A9DA6BFFC4F9ull;

} // namespace natives