#pragma once

// Пользовательская конфигурация плагина: адрес/порт целевого сервера,
// ник игрока и модель персонажа. Читается из rdr2-coop.ini рядом с .asi.
//
// Пример rdr2-coop.ini:
//   server_host = 127.0.0.1
//   server_port = 7777
//   player_name = Arthur
//   ped_model   = 0xCF7E73BE

#include <string>

namespace coopcfg {

struct Config {
    std::string serverHost = "127.0.0.1";
    int         serverPort = 7777;
    std::string playerName = "Player";
    unsigned long long pedModel = 0xCF7E73BEull; // a_m_m_armtownfolk_01
    // P1: репликация мира/NPC (хост -> гость). По умолчанию выключена,
    // пока функция не проверена на двух ПК.
    bool worldSync = false;
    // E3d-a: репликация транспорта хоста (vehicle-only) без полного world_sync.
    // Включается при world_sync=0 && npc_actor=1 && world_vehicles=1.
    bool worldVehicles = true;
    // E3d-a: радиус сканирования транспорта у хоста, м (default 80).
    int npcVehicleRadius = 80;
    // P2: Action v2 — сценарии/сидение/курение/укрытие и др. действий.
    // По умолчанию выключена до теста на двух ПК.
    bool actionSync = false;
    // P2: посадка в транспорт (дилижанс/повозка) удалённого игрока.
    bool vehicleSync = false;
    // P3: хост детектит новые завершённые миссии MissionData и шлёт гостю.
    bool missionSync = false;
    // P3: писать зачёт миссии в свой сейв гостя (опасно, если прогресс разный).
    bool missionCredit = false;
    // P3: перед первым зачётом сделать бэкап сейва гостя. Если бэкап не
    // удался — зачёт не пишется (защита от порчи прогресса).
    bool saveBackup = true;
    // P3: логировать план наград миссии (dry-run, нативы не вызываются).
    bool missionRewardLog = true;
    // R&D: экспериментальная репликация поездов/состава (хост -> гость).
    // По умолчанию выключено: движущийся состав требует отдельной модели.
    bool worldTrains = false;
    // R&D: подробный лог offset/attach для TrainSnapshot (диагностика).
    bool trainDebug = false;
    // R&D E0 (NPC actor sync): каркас синхронизации NPC-акторов. Пока только
    // фасад actor_sync::Tick; по умолчанию выключено (0 = ни нативов, ни логов).
    bool npcActor = false;
    // R&D E-1 (probe): раз в ~2 с логировать 20 ближайших human NPC (только хост).
    // Работает только при npc_actor=1; по умолчанию выключено.
    bool npcDebug = false;
    // E1: максимум реплицируемых актёров-NPC (1..12, default 8) и радиус
    // обнаружения вокруг игроков в метрах (20..100, default 60).
    int npcCap = 8;
    int npcRadius = 60;
    // E3c: максимум реплицируемых животных/бесхозных лошадей (0..12, default 4,
    // 0 = выключено). Занимают свободные слоты roster ПОСЛЕ людей (общий ≤12).
    int npcAnimalCap = 4;
    // E5.2: верхний предел удержания трупа в roster (default 300 = 5 мин,
    // clamp 0..600; 0 = не держать вообще — старое поведение). Пока host-труп
    // существует, гость держит corpse-прокси; despawn при исчезновении трупа
    // или по этому пределу.
    int npcCorpseMaxSec = 300;
    // E2a: применять meta-outfit (внешность) на прокси NPC (default 1).
    // При 0 — поведение E1: только random variation.
    bool npcAppearance = true;
    // E5: host-authoritative урон по NPC (гость шлёт damage, хост применяет).
    // default 1; 0 = рубильник для отладки/отката без пересборки.
    bool npcDamage = true;
    // E6 (v18): синхронизация UI-статуса (карта/пауза/сатчель/магазин) и
    // суффикс в имени над головой. default 1; 0 = выключить presence.
    bool npcPresence = true;
    // E7.1 visitor mode: гость подавляет свою ambient-популяцию (видит только
    // реплицированный мир хоста). npc_visitor_vehicles=0 — оставить трафик.
    bool npcVisitor = true;
    // default 0: транспорт ещё не реплицируется (E3d-a) — оставляем трафик.
    bool npcVisitorVehicles = true; // E3d-a: локальный трафик гостя подавляем
                                     // (машины едут из реплик хоста).
    // Сохранять сценарных/событийных NPC (продавцы/работники/энкаунтеры).
    // 0 = агрессивное подавление (CLEAR_AREA ambient-флагом).
    bool npcVisitorPreserveEvents = true;
    // E7.1: mission-aware visitor — на время миссии/катсцены паузить ТОЛЬКО
    // cleanup ambient-толпы (density-подавление остаётся), default 1.
    bool npcVisitorPauseScenes = true;
    // E3d-c (v19): пассажиры/водители и тягловые лошади у vehicle-прокси.
    bool npcOccupants = true;
    // E9 (task-142): внешность животных/лошадей (asset/tint 0x1E + horse tack
    // 0x1C). default 0 — текущий random variation (host+guest должны совпадать).
    bool npcAnimalAppearance = false;
    // E10 (task-148): внешность лошади под NPC-всадником через ActorOutfit
    // 0x1C с actorId всадника. default 0 — random (host+guest совпадают).
    bool npcMountAppearance = false;
    // P2 (task-139): частота ActorFrames (10..20 Гц; 10 = старое поведение).
    int npcFrameHz = 15;
    // Адаптивность: все стоят — 10 Гц, есть движущиеся — npc_frame_hz.
    bool npcFrameAdaptive = true;
    // P0-2 (task-156): Player Frame throttle (88B каждый тик -> 20 Гц/change).
    int playerFrameHz = 20;          // clamp 10..60
    bool playerFrameChangeOnly = true; // change -> сразу (cap player_frame_hz), idle heartbeat 5 Гц
    // E7.2 smooth NPC motion (A/B):
    // npc_motion_update=1 — in-place update задачи через
    // _UPDATE_TASK_GO_TO_COORD_WITH_OFFSET (совместимость A/B ещё не
    // подтверждена в игре; default 0 = безопасный far-target/reissue).
    bool npcMotionUpdate = true;
    // npc_motion_marker=1 — fallback A2: marker-entity +
    // TASK_FOLLOW_TO_OFFSET_OF_ENTITY (default 0, только после A/B).
    bool npcMotionMarker = false;
    // p2 в GET_SCRIPT_TASK_STATUS (A/B, default true).
    bool npcMotionStatusP2 = true;
};

// Загрузить конфиг из rdr2-coop.ini (рядом с плагином). Если файла нет —
// создать с настройками по умолчанию; при ошибках — оставить дефолты.
Config& Load();

// Разобрать INI из произвольного пути (для тестов/диагностики).
// Возвращает false, если файл не открылся.
bool LoadFromFile(const std::string& path, Config& out);

// Текущий конфиг (после Load()).
Config& Get();

} // namespace coopcfg