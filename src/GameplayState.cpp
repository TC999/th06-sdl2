#include "GameplayState.hpp"

namespace th06::DGS
{
const char *DgsSectionIdToString(DgsSectionId id)
{
    switch (id)
    {
    case DgsSectionId::Envelope:
        return "envelope";
    case DgsSectionId::CoreState:
        return "core";
    case DgsSectionId::RuntimeState:
        return "runtime";
    case DgsSectionId::StageRefs:
        return "stage_refs";
    case DgsSectionId::EnemyRefs:
        return "enemy_refs";
    case DgsSectionId::EclRefs:
        return "ecl_refs";
    case DgsSectionId::Audit:
        return "audit";
    default:
        return "unknown";
    }
}

const char *DgsCoverageKindToString(DgsCoverageKind kind)
{
    switch (kind)
    {
    case DgsCoverageKind::Included:
        return "included";
    case DgsCoverageKind::Excluded:
        return "excluded";
    case DgsCoverageKind::Unresolved:
        return "unresolved";
    default:
        return "unknown";
    }
}
} // namespace th06::DGS
