#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <unordered_map>

#include "imgui/imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui/imgui_internal.h"

#ifdef _WIN32
#include "imgui/imgui_impl_dx9.h"
#else
#include "imgui/imgui_impl_opengl3.h"
#endif

#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "Resources/avatar_ct.h"
#include "Resources/avatar_tt.h"
#include "Resources/skillgroups.h"

#include "fnv.h"
#include "GameData.h"
#include "Interfaces.h"
#include "Memory.h"

#include "SDK/ClassId.h"
#include "SDK/ClientClass.h"
#include "SDK/ClientTools.h"
#include "SDK/Engine.h"
#include "SDK/Entity.h"
#include "SDK/EntityList.h"
#include "SDK/GlobalVars.h"
#include "SDK/Localize.h"
#include "SDK/LocalPlayer.h"
#include "SDK/ModelInfo.h"
#include "SDK/PlayerResource.h"
#include "SDK/Sound.h"
#include "SDK/Steam.h"
#include "SDK/UtlVector.h"
#include "SDK/WeaponId.h"
#include "SDK/WeaponInfo.h"

static Matrix4x4 viewMatrix;
static LocalPlayerData localPlayerData;
static std::vector<PlayerData> playerData;
static std::vector<ObserverData> observerData;
static std::vector<WeaponData> weaponData;
static std::vector<EntityData> entityData;
static std::vector<LootCrateData> lootCrateData;
static std::list<ProjectileData> projectileData;
static std::vector<InfernoData> infernoData;
static std::vector<SmokeData> smokeGrenades;
static BombData bombData;
static std::string gameModeName;
static std::array<std::string, 19> skillGroupNames;
static std::array<std::string, 16> skillGroupNamesDangerzone;

void GameData::update() noexcept
{
    if (static int lastFrame; lastFrame != memory->globalVars->framecount)
        lastFrame = memory->globalVars->framecount;
    else
        return;

    Lock lock;
    observerData.clear();
    weaponData.clear();
    entityData.clear();
    lootCrateData.clear();
    infernoData.clear();

    localPlayerData.update();
    bombData.update();

    if (static bool skillgroupNamesInitialized = false; !skillgroupNamesInitialized) {
        for (std::size_t i = 0; i < skillGroupNames.size(); ++i)
            skillGroupNames[i] = interfaces->localize->findAsUTF8(("RankName_" + std::to_string(i)).c_str());

        for (std::size_t i = 0; i < skillGroupNamesDangerzone.size(); ++i)
            skillGroupNamesDangerzone[i] = interfaces->localize->findAsUTF8(("skillgroup_" + std::to_string(i) + "dangerzone").c_str());

        skillgroupNamesInitialized = true;
    }

    if (!localPlayer) {
        playerData.clear();
        projectileData.clear();
        gameModeName.clear();
        smokeGrenades.clear();
        return;
    }

    std::erase_if(smokeGrenades, [](const auto& smoke) { return interfaces->entityList->getEntityFromHandle(smoke.handle) == nullptr; });
    for (int i = 0; i < memory->smokeHandles->size; ++i) {
        const auto handle = memory->smokeHandles->memory[i];
        const auto smoke = interfaces->entityList->getEntityFromHandle(handle);
        if (!smoke)
            continue;

        if (std::ranges::find(std::as_const(smokeGrenades), handle, &SmokeData::handle) == smokeGrenades.cend())
            smokeGrenades.emplace_back(smoke->getAbsOrigin() + Vector{ 0.0f, 0.0f, 60.0f }, handle);
    }

    gameModeName = memory->getGameModeName(false);
    viewMatrix = interfaces->engine->worldToScreenMatrix();

    const auto observerTarget = localPlayer->getObserverMode() == ObsMode::InEye ? localPlayer->getObserverTarget() : nullptr;

    Entity* entity = nullptr;
    while ((entity = interfaces->clientTools->nextEntity(entity))) {
        if (const auto player = Entity::asPlayer(entity)) {
            if (player == localPlayer.get() || player == observerTarget || player->isGOTV())
                continue;

            if (const auto it = std::ranges::find(playerData, player->handle(), &PlayerData::handle); it != playerData.end()) {
                it->update(player);
            } else {
                playerData.emplace_back(player);
            }

            if (!player->isDormant() && !player->isAlive()) {
                const auto obs = Entity::asPlayer(player->getObserverTarget());
                if (obs)
                    observerData.emplace_back(player, obs, obs == localPlayer.get());
            }
        } else {
            if (entity->isDormant())
                continue;

            if (!entity->isWeapon()) {
                switch (entity->getClientClass()->classId) {
                case ClassId::BaseCSGrenadeProjectile:
                    if (entity->isEffectActive(EF_NODRAW)) {
                        if (const auto it = std::ranges::find(projectileData, entity->handle(), &ProjectileData::handle); it != projectileData.end()) {
                            if (!it->exploded)
                                it->explosionTime = memory->globalVars->realtime;
                            it->exploded = true;
                        }
                        break;
                    }
                    [[fallthrough]];
                case ClassId::BreachChargeProjectile:
                case ClassId::BumpMineProjectile:
                case ClassId::DecoyProjectile:
                case ClassId::MolotovProjectile:
                case ClassId::SensorGrenadeProjectile:
                case ClassId::SmokeGrenadeProjectile:
                case ClassId::SnowballProjectile:
                    if (const auto it = std::ranges::find(projectileData, entity->handle(), &ProjectileData::handle); it != projectileData.end())
                        it->update(entity);
                    else
                        projectileData.emplace_back(entity);
                    break;
                case ClassId::EconEntity:
                case ClassId::Chicken:
                case ClassId::PlantedC4:
                case ClassId::Hostage:
                case ClassId::Dronegun:
                case ClassId::Cash:
                case ClassId::AmmoBox:
                case ClassId::RadarJammer:
                case ClassId::SnowballPile:
                    entityData.emplace_back(entity);
                    break;
                case ClassId::LootCrate:
                    lootCrateData.emplace_back(entity);
                    break;
                case ClassId::Inferno:
                    infernoData.emplace_back(entity);
                    break;
                default:
                    break;
                }
            } else if (entity->ownerEntity() == -1) {
                weaponData.emplace_back(entity);
            }
        }
    }

    std::sort(playerData.begin(), playerData.end());
    std::sort(weaponData.begin(), weaponData.end());
    std::sort(entityData.begin(), entityData.end());
    std::sort(lootCrateData.begin(), lootCrateData.end());

    std::ranges::for_each(projectileData, [](auto& projectile) {
        if (interfaces->entityList->getEntityFromHandle(projectile.handle) == nullptr)
            projectile.exploded = true;
    });

    std::erase_if(projectileData, [](const auto& projectile) { return interfaces->entityList->getEntityFromHandle(projectile.handle) == nullptr
        && (projectile.trajectory.empty() || projectile.trajectory.back().first + 60.0f < memory->globalVars->realtime); });

    std::erase_if(playerData, [](const auto& player) { return interfaces->entityList->getEntityFromHandle(player.handle) == nullptr && player.fadingAlpha() < std::numeric_limits<float>::epsilon(); });
}

void GameData::clearProjectileList() noexcept
{
    Lock lock;
    projectileData.clear();
}

static void clearSkillgroupTextures() noexcept;
static void clearAvatarTextures() noexcept;

struct PlayerAvatar {
    mutable PlayerData::Texture texture;
    std::unique_ptr<std::uint8_t[]> rgba;
};

static std::unordered_map<int, PlayerAvatar> playerAvatars;

void GameData::clearTextures() noexcept
{
    Lock lock;

    clearSkillgroupTextures();
    clearAvatarTextures();
    for (const auto& [handle, avatar] : playerAvatars)
        avatar.texture.clear();
}

void GameData::clearPlayersLastLocation() noexcept
{
    Lock lock;
    std::ranges::for_each(playerData, &std::string::clear, &PlayerData::lastPlaceName);
}

void GameData::clearUnusedAvatars() noexcept
{
    Lock lock;
    std::erase_if(playerAvatars, [](const auto& pair) { return std::ranges::find(std::as_const(playerData), pair.first, &PlayerData::handle) == playerData.cend(); });
}

bool GameData::worldToScreen(const Vector& in, ImVec2& out, bool floor) noexcept
{
    const auto& matrix = viewMatrix;

    const auto w = matrix._41 * in.x + matrix._42 * in.y + matrix._43 * in.z + matrix._44;
    if (w < 0.001f)
        return false;

    out = ImGui::GetIO().DisplaySize / 2.0f;
    out.x *= 1.0f + (matrix._11 * in.x + matrix._12 * in.y + matrix._13 * in.z + matrix._14) / w;
    out.y *= 1.0f - (matrix._21 * in.x + matrix._22 * in.y + matrix._23 * in.z + matrix._24) / w;
    if (floor)
        out = ImFloor(out);
    return true;
}

[[nodiscard]] const LocalPlayerData& GameData::local() noexcept
{
    return localPlayerData;
}

[[nodiscard]] const std::vector<PlayerData>& GameData::players() noexcept
{
    return playerData;
}

[[nodiscard]] const PlayerData* GameData::playerByHandle(int handle) noexcept
{
    const auto it = std::ranges::find(std::as_const(playerData), handle, &PlayerData::handle);
    return it != playerData.cend() ? &(*it) : nullptr;
}

[[nodiscard]] const std::vector<ObserverData>& GameData::observers() noexcept
{
    return observerData;
}

[[nodiscard]] const std::vector<WeaponData>& GameData::weapons() noexcept
{
    return weaponData;
}

[[nodiscard]] const std::vector<EntityData>& GameData::entities() noexcept
{
    return entityData;
}

[[nodiscard]] const std::vector<LootCrateData>& GameData::lootCrates() noexcept
{
    return lootCrateData;
}

[[nodiscard]] const std::list<ProjectileData>& GameData::projectiles() noexcept
{
    return projectileData;
}

[[nodiscard]] const std::vector<InfernoData>& GameData::infernos() noexcept
{
    return infernoData;
}

[[nodiscard]] const std::vector<SmokeData>& GameData::smokes() noexcept
{
    return smokeGrenades;
}

[[nodiscard]] const BombData& GameData::plantedC4() noexcept
{
    return bombData;
}

[[nodiscard]] const std::string& GameData::gameMode() noexcept
{
    return gameModeName;
}

void LocalPlayerData::update() noexcept
{
    if (!localPlayer) {
        exists = false;
        return;
    }

    exists = true;
    alive = localPlayer->isAlive();
    handle = localPlayer->handle();

    if (const auto activeWeapon = localPlayer->getActiveWeapon()) {
        inReload = activeWeapon->isInReload();
        shooting = localPlayer->shotsFired() > 1;
        noScope = activeWeapon->isSniperRifle() && !localPlayer->isScoped();
        nextWeaponAttack = activeWeapon->nextPrimaryAttack();
    }

    fov = localPlayer->fov() ? localPlayer->fov() : localPlayer->defaultFov();
    flashDuration = localPlayer->flashDuration();
    aimPunch = localPlayer->getAimPunch();

    const auto obsMode = localPlayer->getObserverMode();
    if (const auto obs = localPlayer->getObserverTarget(); obs && obsMode != ObsMode::Roaming && obsMode != ObsMode::Deathcam)
        origin = obs->getAbsOrigin();
    else
        origin = localPlayer->getAbsOrigin();
}

BaseData::BaseData(Entity* entity) noexcept : distanceToLocal{ entity->getAbsOrigin().distTo(localPlayerData.origin) }, coordinateFrame { entity->toWorldTransform() }
{
    if (entity->isPlayer()) {
        const auto collideable = entity->getCollideable();
        obbMins = collideable->obbMins();
        obbMaxs = collideable->obbMaxs();
    } else if (const auto model = entity->getModel()) {
        obbMins = model->mins;
        obbMaxs = model->maxs;
    }
}

EntityData::EntityData(Entity* entity) noexcept : BaseData{ entity }
{
    name = [](ClassId classId) {
        switch (classId) {
        case ClassId::EconEntity: return "Defuse Kit";
        case ClassId::Chicken: return "Chicken";
        case ClassId::PlantedC4: return "Planted C4";
        case ClassId::Hostage: return "Hostage";
        case ClassId::Dronegun: return "Sentry";
        case ClassId::Cash: return "Cash";
        case ClassId::AmmoBox: return "Ammo Box";
        case ClassId::RadarJammer: return "Radar Jammer";
        case ClassId::SnowballPile: return "Snowball Pile";
        default: assert(false); return "unknown";
        }
    }(entity->getClientClass()->classId);
}

ProjectileData::ProjectileData(Entity* projectile) noexcept : BaseData{ projectile }
{
    name = [](Entity* projectile) {
        switch (projectile->getClientClass()->classId) {
        case ClassId::BaseCSGrenadeProjectile:
            if (const auto model = projectile->getModel(); model && std::strstr(model->name, "flashbang"))
                return "Flashbang";
            else
                return "HE Grenade";
        case ClassId::BreachChargeProjectile: return "Breach Charge";
        case ClassId::BumpMineProjectile: return "Bump Mine";
        case ClassId::DecoyProjectile: return "Decoy Grenade";
        case ClassId::MolotovProjectile: return "Molotov";
        case ClassId::SensorGrenadeProjectile: return "TA Grenade";
        case ClassId::SmokeGrenadeProjectile: return "Smoke Grenade";
        case ClassId::SnowballProjectile: return "Snowball";
        default: assert(false); return "unknown";
        }
    }(projectile);

    if (const auto thrower = Entity::asPlayer(interfaces->entityList->getEntityFromHandle(projectile->thrower())); thrower && localPlayer) {
        if (thrower == localPlayer.get())
            thrownByLocalPlayer = true;
        else
            thrownByEnemy = thrower->isEnemy();
    }

    handle = projectile->handle();
}

void ProjectileData::update(Entity* projectile) noexcept
{
    static_cast<BaseData&>(*this) = { projectile };

    if (const auto& pos = projectile->getAbsOrigin(); trajectory.empty() || trajectory.back().second != pos)
        trajectory.emplace_back(memory->globalVars->realtime, pos);
}

PlayerData::PlayerData(CSPlayer* entity) noexcept : BaseData{ entity }, userId{ entity->getUserId() }, handle{ entity->handle() }, money{ entity->money() }, team{ entity->getTeamNumber() }, steamID{ entity->getSteamID() }, name{ entity->getPlayerName() }, lastPlaceName{ interfaces->localize->findAsUTF8(entity->lastPlaceName()) }
{
    if (steamID) {
        const auto ctx = interfaces->engine->getSteamAPIContext();
        const auto avatar = ctx->steamFriends->getSmallFriendAvatar(steamID);
        constexpr auto rgbaDataSize = 4 * 32 * 32;

        PlayerAvatar playerAvatar;
        playerAvatar.rgba = std::make_unique<std::uint8_t[]>(rgbaDataSize);
        if (ctx->steamUtils->getImageRGBA(avatar, playerAvatar.rgba.get(), rgbaDataSize))
            playerAvatars[handle] = std::move(playerAvatar);
    }

    update(entity);
}

void PlayerData::update(CSPlayer* entity) noexcept
{
    if (memory->globalVars->framecount % 20 == 0)
        name = entity->getPlayerName();

    const auto idx = entity->index();

    if (*memory->playerResource) {    
        skillgroup = (*memory->playerResource)->competitiveRanking()[idx];
        competitiveWins = (*memory->playerResource)->competitiveWins()[idx];
        armor = (*memory->playerResource)->armor()[idx];
    }

    dormant = entity->isDormant();
    if (dormant) {
        if (const auto pr = *memory->playerResource) {
            alive = pr->getIPlayerResource()->isAlive(idx);
            if (!alive)
                lastContactTime = 0.0f;
            health = pr->getIPlayerResource()->getPlayerHealth(idx);
        }
        return;
    }

    money = entity->money();
    team = entity->getTeamNumber();
    lastPlaceName = interfaces->localize->findAsUTF8(entity->lastPlaceName());
    static_cast<BaseData&>(*this) = { entity };
    origin = entity->getAbsOrigin();
    inViewFrustum = !interfaces->engine->cullBox(obbMins + origin, obbMaxs + origin);
    alive = entity->isAlive();
    lastContactTime = alive ? memory->globalVars->realtime : 0.0f;

    if (localPlayer) {
        enemy = entity->isEnemy();
        visible = inViewFrustum && alive && entity->visibleTo(localPlayer.get());
    }

    constexpr auto isEntityAudible = [](int entityIndex) noexcept {
        for (int i = 0; i < memory->activeChannels->count; ++i)
            if (memory->channels[memory->activeChannels->list[i]].soundSource == entityIndex)
                return true;
        return false;
    };

    audible = isEntityAudible(entity->index());
    spotted = entity->spotted();
    immune = entity->gunGameImmunity();
    flashDuration = entity->flashDuration();
    health = entity->getHealth();

    if (const auto weapon = entity->getActiveWeapon()) {
        audible = audible || isEntityAudible(weapon->index());
        if (const auto weaponInfo = weapon->getWeaponInfo())
            activeWeapon = interfaces->localize->findAsUTF8(weaponInfo->name);
    }

    if (!alive || !inViewFrustum)
        return;

    const auto model = entity->getModel();
    if (!model)
        return;

    const auto studioModel = interfaces->modelInfo->getStudioModel(model);
    if (!studioModel)
        return;

    Matrix3x4 boneMatrices[MAXSTUDIOBONES];
    if (!entity->setupBones(boneMatrices, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory->globalVars->currenttime))
        return;

    bones.clear();
    bones.reserve(20);

    for (int i = 0; i < studioModel->numBones; ++i) {
        const auto bone = studioModel->getBone(i);

        if (!bone || bone->parent == -1 || !(bone->flags & BONE_USED_BY_HITBOX))
            continue;

        bones.emplace_back(boneMatrices[i].origin(), boneMatrices[bone->parent].origin());
    }

    const auto set = studioModel->getHitboxSet(entity->hitboxSet());
    if (!set)
        return;

    const auto headBox = set->getHitbox(Hitbox::Head);

    headMins = headBox->bbMin.transform(boneMatrices[headBox->bone]);
    headMaxs = headBox->bbMax.transform(boneMatrices[headBox->bone]);

    if (headBox->capsuleRadius > 0.0f) {
        headMins -= headBox->capsuleRadius;
        headMaxs += headBox->capsuleRadius;
    }
}

const std::string& PlayerData::getRankName() const noexcept
{
    if (gameModeName == "survival")
        return skillGroupNamesDangerzone[std::size_t(skillgroup) < skillGroupNamesDangerzone.size() ? skillgroup : 0];
    else
        return skillGroupNames[std::size_t(skillgroup) < skillGroupNames.size() ? skillgroup : 0];
}

struct SkillgroupImage {
    template <std::size_t N>
    SkillgroupImage(const std::array<char, N>& png) noexcept : pngData{ png.data() }, pngDataSize{ png.size() } {}

    ImTextureID getTexture() const noexcept
    {
        if (!texture.get()) {
            int width, height;
            stbi_set_flip_vertically_on_load_thread(false);

            if (const auto data = stbi_load_from_memory((const stbi_uc*)pngData, pngDataSize, &width, &height, nullptr, STBI_rgb_alpha)) {
                texture.init(width, height, data);
                stbi_image_free(data);
            } else {
                assert(false);
            }
        }

        return texture.get();
    }

    void clearTexture() const noexcept { texture.clear(); }

private:
    const char* pngData;
    std::size_t pngDataSize;

    mutable PlayerData::Texture texture;
};

static const auto skillgroupImages = std::array<SkillgroupImage, 19>({
Resource::skillgroup0, Resource::skillgroup1, Resource::skillgroup2, Resource::skillgroup3, Resource::skillgroup4, Resource::skillgroup5, Resource::skillgroup6, Resource::skillgroup7,
Resource::skillgroup8, Resource::skillgroup9, Resource::skillgroup10, Resource::skillgroup11, Resource::skillgroup12, Resource::skillgroup13, Resource::skillgroup14, Resource::skillgroup15,
Resource::skillgroup16, Resource::skillgroup17, Resource::skillgroup18 });

static const auto dangerZoneImages = std::array<SkillgroupImage, 16>({
Resource::dangerzone0, Resource::dangerzone1, Resource::dangerzone2, Resource::dangerzone3, Resource::dangerzone4, Resource::dangerzone5, Resource::dangerzone6, Resource::dangerzone7,
Resource::dangerzone8, Resource::dangerzone9, Resource::dangerzone10, Resource::dangerzone11, Resource::dangerzone12, Resource::dangerzone13, Resource::dangerzone14, Resource::dangerzone15 });

static const SkillgroupImage avatarTT{ Resource::avatar_tt };
static const SkillgroupImage avatarCT{ Resource::avatar_ct };

ImTextureID PlayerData::getAvatarTexture() const noexcept
{
    const auto it = std::as_const(playerAvatars).find(handle);
    if (it == playerAvatars.cend())
        return team == Team::TT ? avatarTT.getTexture() : avatarCT.getTexture();

    const auto& avatar = it->second;
    if (!avatar.texture.get())
        avatar.texture.init(32, 32, avatar.rgba.get());
    return avatar.texture.get();
}

static void clearAvatarTextures() noexcept
{
    avatarTT.clearTexture();
    avatarCT.clearTexture();
}

static void clearSkillgroupTextures() noexcept
{
    for (const auto& img : skillgroupImages)
        img.clearTexture();
    for (const auto& img : dangerZoneImages)
        img.clearTexture();
}

ImTextureID PlayerData::getRankTexture() const noexcept
{
    if (gameModeName == "survival")
        return dangerZoneImages[std::size_t(skillgroup) < dangerZoneImages.size() ? skillgroup : 0].getTexture();
    else
        return skillgroupImages[std::size_t(skillgroup) < skillgroupImages.size() ? skillgroup : 0].getTexture();
}

float PlayerData::fadingAlpha() const noexcept
{
    constexpr float fadeTime = 1.50f;
    return std::clamp(1.0f - std::max(memory->globalVars->realtime - lastContactTime - 0.25f, 0.0f) / fadeTime, 0.0f, 1.0f);
}

WeaponData::WeaponData(Entity* entity) noexcept : BaseData{ entity }, clip{ entity->clip() }, reserveAmmo{ entity->reserveAmmoCount() }
{
    if (const auto weaponInfo = entity->getWeaponInfo()) {
        group = [](WeaponType type, WeaponId weaponId) {
            switch (type) {
            case WeaponType::Pistol: return "Pistols";
            case WeaponType::SubMachinegun: return "SMGs";
            case WeaponType::Rifle: return "Rifles";
            case WeaponType::SniperRifle: return "Sniper Rifles";
            case WeaponType::Shotgun: return "Shotguns";
            case WeaponType::Machinegun: return "Machineguns";
            case WeaponType::Grenade: return "Grenades";
            case WeaponType::Melee: return "Melee";
            default:
                switch (weaponId) {
                case WeaponId::C4:
                case WeaponId::Healthshot:
                case WeaponId::BumpMine:
                case WeaponId::ZoneRepulsor:
                case WeaponId::Shield:
                    return "Other";
                default: return "All";
                }
            }
        }(weaponInfo->type, entity->weaponId());
        name = [](WeaponId weaponId) {
            switch (weaponId) {
            default: return "All";

            case WeaponId::Glock: return "Glock-18";
            case WeaponId::Hkp2000: return "P2000";
            case WeaponId::Usp_s: return "USP-S";
            case WeaponId::Elite: return "Dual Berettas";
            case WeaponId::P250: return "P250";
            case WeaponId::Tec9: return "Tec-9";
            case WeaponId::Fiveseven: return "Five-SeveN";
            case WeaponId::Cz75a: return "CZ75-Auto";
            case WeaponId::Deagle: return "Desert Eagle";
            case WeaponId::Revolver: return "R8 Revolver";

            case WeaponId::Mac10: return "MAC-10";
            case WeaponId::Mp9: return "MP9";
            case WeaponId::Mp7: return "MP7";
            case WeaponId::Mp5sd: return "MP5-SD";
            case WeaponId::Ump45: return "UMP-45";
            case WeaponId::P90: return "P90";
            case WeaponId::Bizon: return "PP-Bizon";

            case WeaponId::GalilAr: return "Galil AR";
            case WeaponId::Famas: return "FAMAS";
            case WeaponId::Ak47: return "AK-47";
            case WeaponId::M4A1: return "M4A4";
            case WeaponId::M4a1_s: return "M4A1-S";
            case WeaponId::Sg553: return "SG 553";
            case WeaponId::Aug: return "AUG";

            case WeaponId::Ssg08: return "SSG 08";
            case WeaponId::Awp: return "AWP";
            case WeaponId::G3SG1: return "G3SG1";
            case WeaponId::Scar20: return "SCAR-20";

            case WeaponId::Nova: return "Nova";
            case WeaponId::Xm1014: return "XM1014";
            case WeaponId::Sawedoff: return "Sawed-Off";
            case WeaponId::Mag7: return "MAG-7";

            case WeaponId::M249: return "M249";
            case WeaponId::Negev: return "Negev";

            case WeaponId::Flashbang: return "Flashbang";
            case WeaponId::HeGrenade: return "HE Grenade";
            case WeaponId::SmokeGrenade: return "Smoke Grenade";
            case WeaponId::Molotov: return "Molotov";
            case WeaponId::Decoy: return "Decoy Grenade";
            case WeaponId::IncGrenade: return "Incendiary";
            case WeaponId::TaGrenade: return "TA Grenade";
            case WeaponId::Firebomb: return "Fire Bomb";
            case WeaponId::Diversion: return "Diversion";
            case WeaponId::FragGrenade: return "Frag Grenade";
            case WeaponId::Snowball: return "Snowball";

            case WeaponId::Axe: return "Axe";
            case WeaponId::Hammer: return "Hammer";
            case WeaponId::Spanner: return "Wrench";

            case WeaponId::C4: return "C4";
            case WeaponId::Healthshot: return "Healthshot";
            case WeaponId::BumpMine: return "Bump Mine";
            case WeaponId::ZoneRepulsor: return "Zone Repulsor";
            case WeaponId::Shield: return "Shield";
            }
        }(entity->weaponId());

        displayName = interfaces->localize->findAsUTF8(weaponInfo->name);
    }
}

LootCrateData::LootCrateData(Entity* entity) noexcept : BaseData{ entity }
{
    const auto model = entity->getModel();
    if (!model)
        return;

    name = [](const char* modelName) -> const char* {
        switch (fnv::hashRuntime(modelName)) {
        case fnv::hash("models/props_survival/cases/case_pistol.mdl"): return "Pistol Case";
        case fnv::hash("models/props_survival/cases/case_light_weapon.mdl"): return "Light Case";
        case fnv::hash("models/props_survival/cases/case_heavy_weapon.mdl"): return "Heavy Case";
        case fnv::hash("models/props_survival/cases/case_explosive.mdl"): return "Explosive Case";
        case fnv::hash("models/props_survival/cases/case_tools.mdl"): return "Tools Case";
        case fnv::hash("models/props_survival/cash/dufflebag.mdl"): return "Cash Dufflebag";
        default: return nullptr;
        }
    }(model->name);
}

ObserverData::ObserverData(CSPlayer* entity, CSPlayer* obs, bool targetIsLocalPlayer) noexcept : playerUserId{ entity->getUserId() }, targetUserId{ obs->getUserId() }, targetIsLocalPlayer{ targetIsLocalPlayer} {}

PlayerData::Texture::~Texture()
{
    clear();
}

void PlayerData::Texture::init(int width, int height, const std::uint8_t* data) noexcept
{
    texture = ImGui_CreateTextureRGBA(width, height, data);
}

void PlayerData::Texture::clear() noexcept
{
    if (texture)
        ImGui_DestroyTexture(texture);
    texture = nullptr;
}

InfernoData::InfernoData(Entity* inferno) noexcept
{
    const auto& origin = inferno->getAbsOrigin();

    points.reserve(inferno->fireCount());
    for (int i = 0; i < inferno->fireCount(); ++i) {
        if (inferno->fireIsBurning()[i])
            points.emplace_back(inferno->fireXDelta()[i] + origin.x, inferno->fireYDelta()[i] + origin.y, inferno->fireZDelta()[i] + origin.z);
    }
}

void BombData::update() noexcept
{
    if (memory->plantedC4s->size > 0 && (!*memory->gameRules || (*memory->gameRules)->mapHasBombTarget())) {
        if (const auto bomb = (*memory->plantedC4s)[0]; bomb && bomb->ticking()) {
            blowTime = bomb->blowTime();
            timerLength = bomb->timerLength();
            defuserHandle = bomb->bombDefuser();
            if (defuserHandle != -1) {
                defuseCountDown = bomb->defuseCountDown();
                defuseLength = bomb->defuseLength();
            }

            if (*memory->playerResource) {
                const auto& bombOrigin = bomb->getAbsOrigin();
                bombsite = bombOrigin.distToSqr((*memory->playerResource)->bombsiteCenterA()) > bombOrigin.distToSqr((*memory->playerResource)->bombsiteCenterB());
            }
            return;
        }
    }
    blowTime = 0.0f;
}

SmokeData::SmokeData(const Vector& origin, int handle) noexcept : origin{ origin }, explosionTime{ memory->globalVars->realtime }, handle{ handle } {}
