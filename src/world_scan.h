#pragma once

// Spike 1 (модель A): диагностический сканер мира.
//
// Только чтение: раз в ~секунду перечисляет пулы сущностей через
// экспорты ScriptHookRDR2 (worldGetAllPeds/Vehicles/Objects/Pickups),
// считает сущности рядом с локальным игроком и пишет одну строку в лог.
// Геймплей не меняет, сущности не создаёт и не удаляет.
//
// Нужен, чтобы перед репликацией мира (P1) получить реальные цифры:
// сколько NPC/транспорта/объектов в радиусе, сколько из них mission-entity,
// какой FPS и какой бюджет сети/CPU потребуется.

namespace coop {
namespace world_scan {

// Вызывать каждый тик из coop::Tick() (игровой поток, после получения
// локального player ped). Внутри — троттлинг до ~1 раза в секунду.
void Tick(unsigned int localPlayerPed, unsigned int tickCount);

} // namespace world_scan
} // namespace coop
