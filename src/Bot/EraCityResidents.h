/* Personal Era city residency; no player pointers or database state. */
#ifndef PLAYERBOTS_ERACITYRESIDENTS_H
#define PLAYERBOTS_ERACITYRESIDENTS_H

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

class EraCityResidents
{
public:
    std::uint32_t Assign(std::uint32_t guid, std::uint32_t zone, std::uint32_t team)
    {
        std::lock_guard guard(lock);
        if (auto it = assignments.find(guid); it != assignments.end())
            return it->second;
        for (auto const& city : cities)
        {
            if (city.zone != zone || city.team != team)
                continue;
            unsigned count = 0;
            for (auto const& [id, assigned] : assignments)
                count += assigned == zone;
            if (count < city.limit)
            {
                assignments.emplace(guid, zone);
                return zone;
            }
        }
        return 0;
    }

    std::uint32_t Zone(std::uint32_t guid)
    {
        std::lock_guard guard(lock);
        auto it = assignments.find(guid);
        return it == assignments.end() ? 0 : it->second;
    }

    void Release(std::uint32_t guid)
    {
        std::lock_guard guard(lock);
        assignments.erase(guid);
    }

    void Clear()
    {
        std::lock_guard guard(lock);
        assignments.clear();
    }

private:
    struct City { std::uint32_t zone, team, limit; };
    // AreaDefines.h IDs, TEAM_ALLIANCE=0 / TEAM_HORDE=1.
    static constexpr std::array<City, 3> cities{{{1519, 0, 50}, {1537, 0, 25}, {1637, 1, 50}}};
    // ponytail: one short lock and a scan of at most 125 GUIDs; use per-city counts if this cap grows.
    std::mutex lock;
    std::unordered_map<std::uint32_t, std::uint32_t> assignments;
};

#endif
