#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "AchievementMgr.h"
#include "World.h"
#include "DBCEnums.h"
#include "DBCStructure.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"

class ExplorationBonusMgr
{
public:
    static ExplorationBonusMgr* instance()
    {
        static ExplorationBonusMgr instance;
        return &instance;
    }

    // zone_id -> achievement_id
    std::map<uint32, uint32> areaToAchievementMap;

    // lowGuid -> (achievement_id -> nº de personajes de la cuenta con ese logro)
    std::map<uint32, std::map<uint32, uint32>> playerBonusCache;

    // lowGuid -> (zoneId -> timestamp de la última notificación "falta explorar")
    std::map<uint32, std::map<uint32, time_t>> notifyThrottle;

    void Initialize()
    {
        areaToAchievementMap.clear();

        QueryResult result = WorldDatabase.Query("SELECT zone_id, achievement_id FROM mod_exploration_bonus_zones");

        if (result)
        {
            uint32 count = 0;
            do
            {
                Field* fields = result->Fetch();
                areaToAchievementMap[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
                count++;
            } while (result->NextRow());

            LOG_INFO("server.loading", ">> Exploration Bonus Module: {} zonas cargadas desde mod_exploration_bonus_zones.", count);
        }
        else
        {
            LOG_ERROR("server.loading", ">> Exploration Bonus Module: No se encontraron datos en mod_exploration_bonus_zones. ¡Verifica la DB!");
        }
    }

    uint32 GetAchievementForArea(uint32 zoneId)
    {
        auto it = areaToAchievementMap.find(zoneId);
        return (it != areaToAchievementMap.end()) ? it->second : 0;
    }

    // Carga en caché cuántos personajes de la cuenta tienen cada logro de exploración
    void LoadPlayerCache(uint32 lowGuid, uint32 accountId)
    {
        playerBonusCache.erase(lowGuid);

        if (areaToAchievementMap.empty())
            return;

        // Construir lista de IDs numéricos (sin input de usuario — seguro)
        std::string achivIds;
        for (auto const& [zoneId, achivId] : areaToAchievementMap)
        {
            if (!achivIds.empty())
                achivIds += ',';
            achivIds += std::to_string(achivId);
        }

        std::string query =
            "SELECT ca.achievement, COUNT(*) AS cnt "
            "FROM character_achievement ca "
            "INNER JOIN characters c ON c.guid = ca.guid "
            "WHERE c.account = " + std::to_string(accountId) +
            " AND ca.achievement IN (" + achivIds + ") "
            "GROUP BY ca.achievement";

        QueryResult result = CharacterDatabase.Query(query.c_str());

        auto& cache = playerBonusCache[lowGuid];
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                cache[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
            } while (result->NextRow());
        }

        LOG_DEBUG("module", "ExplorationBonus: Cache cargado para guid={} cuenta={}: {} logros de exploración encontrados.",
                  lowGuid, accountId, cache.size());
    }

    void ClearPlayerCache(uint32 lowGuid)
    {
        playerBonusCache.erase(lowGuid);
        notifyThrottle.erase(lowGuid);
    }

    // Devuelve true (y actualiza timestamp) si se puede mostrar la notificación.
    // Devuelve false si aún está en cooldown.
    bool CheckNotifyThrottle(uint32 lowGuid, uint32 zoneId, uint32 cooldownSecs)
    {
        time_t now = time(nullptr);
        auto& playerMap = notifyThrottle[lowGuid];
        auto it = playerMap.find(zoneId);

        if (it == playerMap.end() || (now - it->second) >= (time_t)cooldownSecs)
        {
            playerMap[zoneId] = now;
            return true;
        }
        return false;
    }

    // Devuelve cuántos personajes de la cuenta tienen este logro (0 = ninguno / no cacheado)
    uint32 GetAccountBonusCount(uint32 lowGuid, uint32 achievementId)
    {
        auto it = playerBonusCache.find(lowGuid);
        if (it == playerBonusCache.end())
            return 0;
        auto it2 = it->second.find(achievementId);
        return (it2 != it->second.end()) ? it2->second : 0;
    }

    // Calcula el multiplicador final según el modo activo
    float ComputeMultiplier(uint32 lowGuid, uint32 achievementId, float baseMultiplier, bool accountWide, uint32& outCount)
    {
        outCount = 1;
        if (!accountWide)
            return baseMultiplier;

        uint32 count = GetAccountBonusCount(lowGuid, achievementId);
        if (count > 0)
        {
            outCount = count;
            return 1.0f + (baseMultiplier - 1.0f) * (float)count;
        }
        return baseMultiplier;
    }
};

class ExplorationBonusWorld : public WorldScript
{
public:
    ExplorationBonusWorld() : WorldScript("ExplorationBonusWorld") { }

    void OnStartup() override
    {
        ExplorationBonusMgr::instance()->Initialize();
    }
};

class ExplorationBonusPlayer : public PlayerScript
{
public:
    ExplorationBonusPlayer() : PlayerScript("ExplorationBonusPlayer") { }

    // -------------------------------------------------------------------------
    // Login / Logout — gestión de caché
    // -------------------------------------------------------------------------
    void OnPlayerLogin(Player* player) override
    {
        if (!sConfigMgr->GetOption<bool>("ExplorationBonus.Enable", true, false))
            return;

        if (!sConfigMgr->GetOption<bool>("ExplorationBonus.AccountWide", true, false))
            return;

        ExplorationBonusMgr::instance()->LoadPlayerCache(
            player->GetGUID().GetCounter(),
            player->GetSession()->GetAccountId());
    }

    void OnPlayerLogout(Player* player) override
    {
        ExplorationBonusMgr::instance()->ClearPlayerCache(player->GetGUID().GetCounter());
    }

    // -------------------------------------------------------------------------
    // Cambio de zona — informar al jugador del estado del bonus
    // -------------------------------------------------------------------------
    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!sConfigMgr->GetOption<bool>("ExplorationBonus.Enable", true, false))
            return;

        if (!sConfigMgr->GetOption<bool>("ExplorationBonus.Announce", true, false))
            return;

        uint32 achievementId = ExplorationBonusMgr::instance()->GetAchievementForArea(newZone);
        if (!achievementId)
            return;

        float baseMultiplier = sConfigMgr->GetOption<float>("ExplorationBonus.Multiplier", 1.15f, false);
        bool accountWide     = sConfigMgr->GetOption<bool>("ExplorationBonus.AccountWide", true, false);

        if (player->HasAchieved(achievementId))
        {
            uint32 count;
            float multiplier = ExplorationBonusMgr::instance()->ComputeMultiplier(
                player->GetGUID().GetCounter(), achievementId, baseMultiplier, accountWide, count);
            int32 bonusPercent = (int32)((multiplier - 1.0f) * 100.0f + 0.5f);

            if (accountWide && count > 1)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff71d5ff[Exploración]|r Bonus de XP |cff00ff00+{}%|r activo. "
                    "|cffFFD700({} personajes han explorado esta zona)|r",
                    bonusPercent, count);
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff71d5ff[Exploración]|r Bonus de XP |cff00ff00+{}%|r activo en esta zona.",
                    bonusPercent);
            }
        }
        else
        {
            // Mostrar solo una vez al entrar, o cada N segundos de cooldown
            uint32 cooldown = sConfigMgr->GetOption<uint32>("ExplorationBonus.ZoneNotifyCooldown", 300, false);
            if (!ExplorationBonusMgr::instance()->CheckNotifyThrottle(
                    player->GetGUID().GetCounter(), newZone, cooldown))
                return;

            int32 baseBonusPercent = (int32)((baseMultiplier - 1.0f) * 100.0f + 0.5f);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff71d5ff[Exploración]|r ¡Explora toda esta zona para desbloquear el bonus de |cff00ff00+{}%|r XP!",
                baseBonusPercent);
        }
    }

    // -------------------------------------------------------------------------
    // Bonus de XP aplicado
    // -------------------------------------------------------------------------
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (!sConfigMgr->GetOption<bool>("ExplorationBonus.Enable", true, false))
            return;

        bool sourceAllowed = false;
        switch (xpSource)
        {
            case XPSOURCE_KILL:
                sourceAllowed = sConfigMgr->GetOption<bool>("ExplorationBonus.ApplyToKills", true, false);
                break;
            case XPSOURCE_QUEST:
            case XPSOURCE_QUEST_DF:
                sourceAllowed = sConfigMgr->GetOption<bool>("ExplorationBonus.ApplyToQuests", true, false);
                break;
            case XPSOURCE_EXPLORE:
                sourceAllowed = sConfigMgr->GetOption<bool>("ExplorationBonus.ApplyToExplore", false, false);
                break;
            case XPSOURCE_BATTLEGROUND:
                sourceAllowed = sConfigMgr->GetOption<bool>("ExplorationBonus.ApplyToBattleground", false, false);
                break;
            default:
                break;
        }

        if (!sourceAllowed)
            return;

        uint32 zoneId = player->GetZoneId();
        uint32 achievementId = ExplorationBonusMgr::instance()->GetAchievementForArea(zoneId);

        if (!achievementId || !player->HasAchieved(achievementId))
            return;

        float baseMultiplier = sConfigMgr->GetOption<float>("ExplorationBonus.Multiplier", 1.15f, false);
        bool accountWide     = sConfigMgr->GetOption<bool>("ExplorationBonus.AccountWide", true, false);

        uint32 count;
        float multiplier = ExplorationBonusMgr::instance()->ComputeMultiplier(
            player->GetGUID().GetCounter(), achievementId, baseMultiplier, accountWide, count);

        uint32 oldAmount = amount;
        uint32 bonus = (uint32)((float)oldAmount * (multiplier - 1.0f) + 0.5f);
        if (bonus == 0 && multiplier > 1.0f)
            bonus = 1;

        amount = oldAmount + bonus;

        LOG_DEBUG("module", "ExplorationBonus: Player {} | Zone {} | Achiv: {} | Source: {} | Stacks: {} | XP: {} -> {} (+{})",
                  player->GetName(), zoneId, achievementId, (uint32)xpSource, count, oldAmount, amount, bonus);

        if (sConfigMgr->GetOption<bool>("ExplorationBonus.Announce", true, false))
        {
            int32 bonusPercent = (int32)((multiplier - 1.0f) * 100.0f + 0.5f);
            if (accountWide && count > 1)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff71d5ff[Exploración]|r |cff00ff00+{} XP|r |cff71d5ff(+{}%)|r |cffFFD700[{} personajes · x{} bonus]|r",
                    bonus, bonusPercent, count, count);
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff71d5ff[Exploración]|r Bonus de experiencia aplicado: |cff00ff00+{} XP|r |cff71d5ff(+{}%)|r",
                    bonus, bonusPercent);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Al completar un logro de exploración
    // -------------------------------------------------------------------------
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        uint32 achievementId = ExplorationBonusMgr::instance()->GetAchievementForArea(player->GetZoneId());

        if (!achievementId || achievement->ID != achievementId)
            return;

        bool accountWide = sConfigMgr->GetOption<bool>("ExplorationBonus.AccountWide", true, false);

        // Recargar caché — este personaje acaba de sumar uno más al contador
        if (accountWide)
        {
            ExplorationBonusMgr::instance()->LoadPlayerCache(
                player->GetGUID().GetCounter(),
                player->GetSession()->GetAccountId());
        }

        float baseMultiplier = sConfigMgr->GetOption<float>("ExplorationBonus.Multiplier", 1.15f, false);
        uint32 count;
        float multiplier = ExplorationBonusMgr::instance()->ComputeMultiplier(
            player->GetGUID().GetCounter(), achievementId, baseMultiplier, accountWide, count);
        int32 bonusPercent = (int32)((multiplier - 1.0f) * 100.0f + 0.5f);

        if (accountWide && count > 1)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff71d5ff[Exploración]|r ¡Exploración Completa! Bonus acumulado: |cff00ff00+{}%|r "
                "|cffFFD700({} personajes han explorado esta zona)|r",
                bonusPercent, count);
        }
        else
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff71d5ff[Exploración]|r ¡Exploración Completa! Bonus de XP del |cff00ff00+{}%|r desbloqueado en esta zona.",
                bonusPercent);
        }

        player->CastSpell(player, 43308, true);
    }
};

void AddSC_mod_exploration_bonus()
{
    new ExplorationBonusWorld();
    new ExplorationBonusPlayer();
}
