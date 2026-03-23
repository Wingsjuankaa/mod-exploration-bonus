#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "AchievementMgr.h"
#include "World.h"
#include "DBCEnums.h"
#include "DBCStructure.h"
#include "DBCStores.h"

class ExplorationBonusMgr
{
public:
    static ExplorationBonusMgr* instance()
    {
        static ExplorationBonusMgr instance;
        return &instance;
    }

    std::map<uint32, uint32> areaToAchievementMap;

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
                uint32 zoneId = fields[0].Get<uint32>();
                uint32 achievementId = fields[1].Get<uint32>();

                areaToAchievementMap[zoneId] = achievementId;
                count++;
            } while (result->NextRow());

            LOG_INFO("server.loading", ">> Exploration Bonus Module: {} zonas cargadas desde mod_exploration_bonus_zones.", count);
        }
        else
        {
            LOG_ERROR("server.loading", ">> Exploration Bonus Module: No se encontraron datos en mod_exploration_bonus_zones. ¡Verifica la DB!");
        }
    }
    uint32 GetAchievementForArea(uint32 areaId)
    {
        auto it = areaToAchievementMap.find(areaId);
        if (it != areaToAchievementMap.end())
            return it->second;
        return 0;
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

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        uint32 areaId = player->GetAreaId();
        uint32 zoneId = player->GetZoneId();
        
        uint32 achievementId = ExplorationBonusMgr::instance()->GetAchievementForArea(areaId);
        if (!achievementId)
            achievementId = ExplorationBonusMgr::instance()->GetAchievementForArea(zoneId);

        bool hasAchieved = (achievementId != 0 && player->HasAchieved(achievementId));

        // LOG DETALLADO PARA DEBUG (Ver en consola)
        LOG_INFO("module", "ExplorationBonus: Player {} | Area {}/Zone {} | Achiv: {} | Status: {} | Source: {}", 
                 player->GetName(), areaId, zoneId, achievementId, (hasAchieved ? "YES" : "NO"), (uint32)xpSource);

        if (!sConfigMgr->GetOption<bool>("ExplorationBonus.Enable", true, false))
            return;

        if (hasAchieved)
        {
            float multiplier = sConfigMgr->GetOption<float>("ExplorationBonus.Multiplier", 1.15f, false);
            uint32 oldAmount = amount;
            amount = (uint32)(amount * multiplier);

            LOG_INFO("module", "ExplorationBonus: Applied! Multiplier: {} | XP: {} -> {}", multiplier, oldAmount, amount);

            if (sConfigMgr->GetOption<bool>("ExplorationBonus.Announce", true, false) && amount > oldAmount)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("|cff71d5ff[Exploración]|r Bonus de experiencia aplicado: |cff00ff00+{} XP|r |cff71d5ff(+15%)|r", (amount - oldAmount));
            }
        }
    }

    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        uint32 zoneAchievement = ExplorationBonusMgr::instance()->GetAchievementForArea(player->GetZoneId());
        
        if (achievement->ID == zoneAchievement)
        {
            ChatHandler(player->GetSession()).SendNotification("¡Exploracion Completa! Bonus de XP del 15% desbloqueado en esta zona.");
            player->CastSpell(player, 43308, true); 
        }
    }
};

void AddSC_mod_exploration_bonus()
{
    new ExplorationBonusWorld();
    new ExplorationBonusPlayer();
}
