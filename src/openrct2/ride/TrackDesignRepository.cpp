/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TrackDesignRepository.h"

#include "../Context.h"
#include "../PlatformEnvironment.h"
#include "../core/Collections.hpp"
#include "../core/Console.hpp"
#include "../core/File.h"
#include "../core/FileIndex.hpp"
#include "../core/FileStream.h"
#include "../core/FlagHolder.hpp"
#include "../core/Json.hpp"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../localisation/LocalisationService.h"
#include "../object/ObjectRepository.h"
#include "../ride/RideData.h"
#include "../world/Map.h"
#include "TrackData.h"
#include "TrackDesign.h"
#include "ted/TrackElementDescriptor.h"

using namespace OpenRCT2;

enum TrackRepoItemFlag : uint8_t
{
    readOnly,
};
using TrackRepoItemFlags = FlagHolder<uint32_t, TrackRepoItemFlag>;

struct TrackRepositoryItem
{
    std::string Name;
    std::string Path;
    ride_type_t RideType = kRideTypeNull;
    std::string ObjectEntry;
    TrackRepoItemFlags flags{};
};

std::string GetNameFromTrackPath(const std::string& path)
{
    std::string name = Path::GetFileNameWithoutExtension(path);
    // The track name should be the file name until the first instance of a dot
    name = name.substr(0, name.find_first_of('.'));
    return name;
}

static std::string GetTrackDesignSource(const std::string& path, const std::vector<std::string>& searchPaths)
{
    if (searchPaths.size() > 0 && String::startsWith(path, searchPaths[0]))
    {
        return "rct1";
    }
    if (searchPaths.size() > 1 && String::startsWith(path, searchPaths[1]))
    {
        return "rct2";
    }
    return "user";
}

static void WriteTrackDesignJson(const TrackDesign& td, const std::string& path, const std::string& source)
{
    std::string name = GetNameFromTrackPath(path);

    json_t root = json_t::object();

    root["meta"] = {
        { "source", source },
        { "name", name },
        { "path", path },
        { "version", static_cast<int>(td.version) },
    };

    root["ride"] = {
        { "rideType", td.trackAndVehicle.rtdIndex },
        { "vehicleObjectIdentifier", td.trackAndVehicle.vehicleObject.GetName() },
        { "numberOfTrains", td.trackAndVehicle.numberOfTrains },
        { "numberOfCarsPerTrain", td.trackAndVehicle.numberOfCarsPerTrain },
    };

    root["operation"] = {
        { "rideMode", static_cast<int>(td.operation.rideMode) },
        { "liftHillSpeed", td.operation.liftHillSpeed },
        { "numCircuits", td.operation.numCircuits },
        { "operationSetting", td.operation.operationSetting },
        { "departFlags", td.operation.departFlags },
        { "minWaitingTime", td.operation.minWaitingTime },
        { "maxWaitingTime", td.operation.maxWaitingTime },
    };

    root["appearance"] = {
        { "trackColours", nullptr },
        { "stationObjectIdentifier", td.appearance.stationObjectIdentifier },
        { "vehicleColourSettings", static_cast<int>(td.appearance.vehicleColourSettings) },
        { "vehicleColours", nullptr },
    };

    root["statistics"] = {
        { "excitement", td.statistics.ratings.excitement },
        { "intensity", td.statistics.ratings.intensity },
        { "nausea", td.statistics.ratings.nausea },
        { "maxSpeed", td.statistics.maxSpeed },
        { "averageSpeed", td.statistics.averageSpeed },
        { "rideLength", td.statistics.rideLength },
        { "maxPositiveVerticalG", td.statistics.maxPositiveVerticalG },
        { "maxNegativeVerticalG", td.statistics.maxNegativeVerticalG },
        { "maxLateralG", td.statistics.maxLateralG },
        { "totalAirTime", td.statistics.totalAirTime },
        { "drops", td.statistics.drops },
        { "highestDropHeight", td.statistics.highestDropHeight },
        { "inversions", td.statistics.inversions },
        { "holes", td.statistics.holes },
        { "upkeepCost", td.statistics.upkeepCost },
        { "spaceRequired", { td.statistics.spaceRequired.x, td.statistics.spaceRequired.y } },
    };

    json_t trackElements = json_t::array();
    CoordsXYZ newCoords{ 0, 0, 0 };
    uint8_t rotation = 0;
    for (const auto& tdte : td.trackElements)
    {
        const auto& ted = TrackMetadata::GetTrackElementDescriptor(tdte.type);

        json_t element = json_t::object();

        // XYZD
        element["x"] = newCoords.x;
        element["y"] = newCoords.y;
        element["z"] = newCoords.z - ted.coordinates.zBegin;
        element["direction"] = rotation;

        // TrackDesignTrackElement
        element["trackType"] = static_cast<int>(tdte.type);
        element["trackPlaceFlags"] = tdte.flags.holder;
        element["hasChain"] = tdte.flags.has(TrackDesignTrackElementFlag::hasChain);
        element["isInverted"] = tdte.flags.has(TrackDesignTrackElementFlag::isInverted);
        element["colour"] = tdte.colourScheme;
        element["stationIndex"] = tdte.stationIndex.ToUnderlying();
        element["brakeSpeed"] = tdte.brakeBoosterSpeed;
        element["seatRotation"] = tdte.seatRotation;

        trackElements.push_back(element);

        auto offsetAndRotatedTrack = CoordsXY{ newCoords } + CoordsXY{ ted.coordinates.x, ted.coordinates.y }.Rotate(rotation);
        newCoords = { offsetAndRotatedTrack, newCoords.z - ted.coordinates.zBegin + ted.coordinates.zEnd };
        rotation = (rotation + ted.coordinates.rotationEnd - ted.coordinates.rotationBegin) & 3;
        if (ted.coordinates.rotationEnd & (1 << 2))
        {
            rotation |= (1 << 2);
        }
        else
        {
            newCoords += CoordsDirectionDelta[rotation];
        }
    }
    root["trackElements"] = trackElements;

    // json_t scenery = json_t::array();
    // for (const auto& s : td.sceneryElements)
    // {
    //     scenery.push_back({ { "x", s.loc.x }, { "y", s.loc.y }, { "z", s.loc.z }, { "flags", s.flags } });
    // }
    // root["sceneryElements"] = scenery;

    // json_t entrances = json_t::array();
    // for (const auto& e : td.entranceElements)
    // {
    //     entrances.push_back({ { "x", e.location.x }, { "y", e.location.y }, { "z", e.location.z }, { "isExit", e.isExit } });
    // }
    // root["entranceElements"] = entrances;

    // json_t maze = json_t::array();
    // for (const auto& m : td.mazeElements)
    // {
    //     maze.push_back({ { "x", m.location.x }, { "y", m.location.y }, { "mazeEntry", m.mazeEntry } });
    // }
    // root["mazeElements"] = maze;

    std::string outDir = Path::Combine("out", source);
    Path::CreateDirectory(outDir);
    std::string outPath = Path::Combine(outDir, name + ".json");
    Json::WriteToFile(outPath, root);
}

class TrackDesignFileIndex final : public FileIndex<TrackRepositoryItem>
{
private:
    static constexpr uint32_t kMagicNumber = 0x58444954; // TIDX
    static constexpr uint16_t kVersion = 5;
    static constexpr auto kPattern = "*.td4;*.td6;*.td7";

public:
    explicit TrackDesignFileIndex(const IPlatformEnvironment& env)
        : FileIndex(
              "track design index", kMagicNumber, kVersion, env.GetFilePath(PathId::cacheTracks), std::string(kPattern),
              std::vector<std::string>({
                  env.GetDirectoryPath(DirBase::rct1, DirId::trackDesigns),
                  env.GetDirectoryPath(DirBase::rct2, DirId::trackDesigns),
                  env.GetDirectoryPath(DirBase::user, DirId::trackDesigns),
              }))
    {
    }

public:
    std::optional<TrackRepositoryItem> Create(int32_t, const std::string& path) const override
    {
        auto td = TrackDesignImport(path.c_str());
        if (td != nullptr)
        {
            auto source = GetTrackDesignSource(path, SearchPaths);
            std::cout << "Exporting track design \"" << GetNameFromTrackPath(path) << "\" from source " << source << " to JSON"
                      << std::endl;
            WriteTrackDesignJson(*td, path, source);

            TrackRepositoryItem item{};
            item.Name = GetNameFromTrackPath(path);
            item.Path = path;
            item.RideType = td->trackAndVehicle.rtdIndex;
            item.ObjectEntry = std::string(td->trackAndVehicle.vehicleObject.Entry.name, 8);
            if (IsTrackReadOnly(path))
            {
                item.flags.set(TrackRepoItemFlag::readOnly);
            }
            return item;
        }

        return std::nullopt;
    }

protected:
    void Serialise(DataSerialiser& ds, const TrackRepositoryItem& item) const override
    {
        ds << item.Name;
        ds << item.Path;
        ds << item.RideType;
        ds << item.ObjectEntry;
        ds << item.flags.holder;
    }

private:
    bool IsTrackReadOnly(const std::string& path) const
    {
        return String::startsWith(path, SearchPaths[0]) || String::startsWith(path, SearchPaths[1]);
    }
};

class TrackDesignRepository final : public ITrackDesignRepository
{
private:
    IPlatformEnvironment& _env;
    TrackDesignFileIndex const _fileIndex;
    std::vector<TrackRepositoryItem> _items;

public:
    explicit TrackDesignRepository(IPlatformEnvironment& env)
        : _env(env)
        , _fileIndex(env)
    {
    }

    size_t GetCount() const override
    {
        return _items.size();
    }

    /**
     *
     * @param entry The entry name to count the track list of. Leave empty to count track list for the non-separated types (e.g.
     * Hyper-Twister, Car Ride)
     */
    size_t GetCountForObjectEntry(ride_type_t rideType, const std::string& entry) const override
    {
        size_t count = 0;
        const auto& repo = GetContext()->GetObjectRepository();

        for (const auto& item : _items)
        {
            if (item.RideType != rideType)
            {
                continue;
            }

            bool entryIsNotSeparate = false;
            if (entry.empty())
            {
                const ObjectRepositoryItem* ori = repo.FindObjectLegacy(item.ObjectEntry.c_str());

                if (ori == nullptr || !GetRideTypeDescriptor(rideType).flags.has(RtdFlag::listVehiclesSeparately))
                    entryIsNotSeparate = true;
            }

            if (entryIsNotSeparate || String::iequals(item.ObjectEntry, entry))
            {
                count++;
            }
        }
        return count;
    }

    /**
     *
     * @param entry The entry name to build a track list for. Leave empty to build track list for the non-separated types (e.g.
     * Hyper-Twister, Car Ride)
     */
    std::vector<TrackDesignFileRef> GetItemsForObjectEntry(ride_type_t rideType, const std::string& entry) const override
    {
        std::vector<TrackDesignFileRef> refs;
        const auto& repo = GetContext()->GetObjectRepository();

        for (const auto& item : _items)
        {
            if (item.RideType != rideType)
            {
                continue;
            }

            bool entryIsNotSeparate = false;
            if (entry.empty())
            {
                const ObjectRepositoryItem* ori = repo.FindObjectLegacy(item.ObjectEntry.c_str());

                if (ori == nullptr || !GetRideTypeDescriptor(rideType).flags.has(RtdFlag::listVehiclesSeparately))
                    entryIsNotSeparate = true;
            }

            if (entryIsNotSeparate || String::iequals(item.ObjectEntry, entry))
            {
                TrackDesignFileRef ref;
                ref.name = GetNameFromTrackPath(item.Path);
                ref.path = item.Path;
                refs.push_back(ref);
            }
        }

        return refs;
    }

    void Scan(int32_t language) override
    {
        _items.clear();
        auto trackDesigns = _fileIndex.Rebuild(language);
        for (const auto& td : trackDesigns)
        {
            _items.push_back(td);
        }

        SortItems();
    }

    bool Delete(const std::string& path) override
    {
        bool result = false;
        size_t index = GetTrackIndex(path);
        if (index != SIZE_MAX)
        {
            const TrackRepositoryItem* item = &_items[index];
            if (!item->flags.has(TrackRepoItemFlag::readOnly))
            {
                if (File::Delete(path))
                {
                    _items.erase(_items.begin() + index);
                    result = true;
                }
            }
        }
        return result;
    }

    std::string Rename(const std::string& path, const std::string& newName) override
    {
        std::string result;
        size_t index = GetTrackIndex(path);
        if (index != SIZE_MAX)
        {
            TrackRepositoryItem* item = &_items[index];
            if (!item->flags.has(TrackRepoItemFlag::readOnly))
            {
                std::string directory = Path::GetDirectory(path);
                std::string newPath = Path::Combine(directory, newName + Path::GetExtension(path));
                if (File::Move(path, newPath))
                {
                    item->Name = newName;
                    item->Path = newPath;
                    SortItems();
                    result = std::move(newPath);
                }
            }
        }
        return result;
    }

    std::string Install(const std::string& path, const std::string& name) override
    {
        std::string result;
        std::string installDir = _env.GetDirectoryPath(DirBase::user, DirId::trackDesigns);

        std::string newPath = Path::Combine(installDir, name + Path::GetExtension(path));
        if (File::Copy(path, newPath, false))
        {
            auto language = LocalisationService_GetCurrentLanguage();
            if (auto td = _fileIndex.Create(language, newPath); td.has_value())
            {
                _items.push_back(std::move(td.value()));
                SortItems();
                result = path;
            }
        }
        return result;
    }

private:
    void SortItems()
    {
        std::sort(_items.begin(), _items.end(), [](const TrackRepositoryItem& a, const TrackRepositoryItem& b) -> bool {
            if (a.RideType != b.RideType)
            {
                return a.RideType < b.RideType;
            }
            return String::logicalCmp(a.Name.c_str(), b.Name.c_str()) < 0;
        });
    }

    size_t GetTrackIndex(const std::string& path) const
    {
        for (size_t i = 0; i < _items.size(); i++)
        {
            if (Path::Equals(_items[i].Path, path))
            {
                return i;
            }
        }
        return SIZE_MAX;
    }

    TrackRepositoryItem* GetTrackItem(const std::string& path)
    {
        TrackRepositoryItem* result = nullptr;
        size_t index = GetTrackIndex(path);
        if (index != SIZE_MAX)
        {
            result = &_items[index];
        }
        return result;
    }
};

std::unique_ptr<ITrackDesignRepository> CreateTrackDesignRepository(IPlatformEnvironment& env)
{
    return std::make_unique<TrackDesignRepository>(env);
}

void TrackRepositoryScan()
{
    ITrackDesignRepository* repo = GetContext()->GetTrackDesignRepository();
    repo->Scan(LocalisationService_GetCurrentLanguage());
}

bool TrackRepositoryDelete(const u8string& path)
{
    ITrackDesignRepository* repo = GetContext()->GetTrackDesignRepository();
    return repo->Delete(path);
}

bool TrackRepositoryRename(const u8string& path, const u8string& newName)
{
    ITrackDesignRepository* repo = GetContext()->GetTrackDesignRepository();
    std::string newPath = repo->Rename(path, newName);
    return !newPath.empty();
}

bool TrackRepositoryInstall(const u8string& srcPath, const u8string& name)
{
    ITrackDesignRepository* repo = GetContext()->GetTrackDesignRepository();
    std::string newPath = repo->Install(srcPath, name);
    return !newPath.empty();
}
