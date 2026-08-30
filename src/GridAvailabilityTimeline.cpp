#include "dispatcher/GridAvailabilityTimeline.h"

bool isIntervalFullyAvailable(const QVector<GridStateChange> &timeline, const QDateTime &start,
                               double durationHours, bool defaultBeforeTimeline)
{
    const QDateTime end = start.addMSecs(static_cast<qint64>(durationHours * 3600000.0));

    // Состояние на начало интервала - последний перелом не позже start,
    // либо состояние "до всех данных", если такого перелома нет.
    bool stateAtStart = defaultBeforeTimeline;
    for (const GridStateChange &change : timeline)
    {
        if (change.at > start)
            break; // timeline отсортирован по возрастанию - дальше все позже start
        stateAtStart = change.availableFrom;
    }
    if (!stateAtStart)
        return false;

    // Любой перелом СТРОГО внутри интервала в сторону "недоступно" делает
    // недоступным весь интервал, даже если отключение длилось лишь часть его.
    for (const GridStateChange &change : timeline)
    {
        if (change.at <= start)
            continue;
        if (change.at >= end)
            break;
        if (!change.availableFrom)
            return false;
    }
    return true;
}

QVector<GridStateChange> buildTimelineFromPoints(const QVector<GridAvailabilityPoint> &points)
{
    QVector<GridStateChange> timeline;
    timeline.reserve(points.size());
    for (const GridAvailabilityPoint &p : points)
        timeline.push_back({p.timestamp, p.gridOn});
    return timeline;
}
