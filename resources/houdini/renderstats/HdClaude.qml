// hdClaude's page in Houdini's Render Stats panel.
//
// Named by "qmlTemplate" in UsdRenderers.json, which Houdini resolves against
// that file's directory. `stats` is the delegate's GetRenderStats()
// dictionary, polled by Houdini; every figure here was recorded by the
// renderer when it did the work, so drawing this page costs the render nothing
// (docs/render-stats.md). Built from the same components as Houdini's own
// Karma page, so it looks like the rest of the panel.

import QtQuick
import houdini.pluto
import houdini.ui
import houdini.info
import houdini.renderstats

Item {
    id: root

    property real hSpacing: 8
    readonly property real availableWidth: root.width - (col.leftPadding + col.rightPadding)
    readonly property real halfWidth: (root.availableWidth - root.hSpacing) / 2
    readonly property real fractionDone: stats && stats.percentDone !== undefined ? stats.percentDone / 100.0 : 0.0
    readonly property real elapsed: stats && stats.totalClockTime !== undefined ? stats.totalClockTime : 0.0

    function value(key) {
        return stats && stats[key] !== undefined ? stats[key] : 0
    }

    width: parent.width
    height: col.height

    Column {
        id: col
        width: root.availableWidth
        spacing: 8
        leftPadding: 8
        rightPadding: 8
        bottomPadding: 8

        // --- Progress ------------------------------------------------------
        Column {
            width: parent.width
            spacing: 4

            Row {
                width: parent.width - 16
                spacing: root.hSpacing

                SimpleTile {
                    width: root.halfWidth
                    label: "Elapsed"
                    Duration {
                        seconds: root.elapsed
                        decimalPlaces: 0
                        showZeros: true
                        hms: false
                        color: Theme.fg
                        size: Typography.xlarge
                    }
                }
                SimpleTile {
                    width: root.halfWidth - 24
                    label: "ETA"
                    Duration {
                        seconds: root.fractionDone > 0.0 ? root.elapsed / root.fractionDone - root.elapsed : 0.0
                        decimalPlaces: 0
                        showZeros: true
                        hms: false
                        color: Theme.fg
                        size: Typography.xlarge
                    }
                }
            }
            ProgressBar {
                width: parent.width
                value: root.fractionDone
            }
        }

        ItemGrid {
            width: parent.width
            cellWidth: 110
            cellHeight: 40

            SimpleTile {
                label: "Samples"
                Text {
                    text: `${root.value("samplesCompleted")} / ${root.value("targetSamples")}`
                    color: Theme.fg
                    size: Typography.large
                }
            }
            SimpleTile {
                label: "Device"
                Text {
                    text: stats && stats.device ? stats.device : ""
                    elide: Text.ElideRight
                    width: parent.width
                }
            }
            SimpleTile {
                label: "Device Memory"
                BriefNumber {
                    number: root.value("deviceBytesUsed")
                    memory: true
                    color: Theme.fg
                    size: Typography.large
                }
            }
            SimpleTile {
                label: "Peak"
                BriefNumber {
                    number: root.value("deviceBytesPeak")
                    memory: true
                    color: Theme.fg
                    size: Typography.large
                }
            }
            SimpleTile {
                label: "Available"
                BriefNumber {
                    number: root.value("deviceBytesAvailable")
                    memory: true
                    color: Theme.fg
                    size: Typography.large
                }
            }
        }

        // --- Instancing ----------------------------------------------------
        //
        // hdClaude flattens instancing, nested or not, into one placement per
        // copy. These say how long that list is, how it was made, and what one
        // entry of it costs on the device.
        ExpandingTile {
            width: parent.width
            label: "Instancing"
            initiallyExpanded: true

            Column {
                width: parent.width
                spacing: 6

                ItemGrid {
                    width: parent.width
                    cellWidth: 110
                    cellHeight: 40

                    SimpleTile {
                        label: "Placements"
                        BriefNumber {
                            number: root.value("instances")
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "Prototypes"
                        BriefNumber {
                            number: root.value("prototypes")
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "Per Prototype"
                        Text {
                            // Not Number(...): the pluto `Number` type
                            // shadows the JavaScript one in a binding.
                            text: (+root.value("placementsPerPrototype")).toFixed(1)
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "Nesting Depth"
                        Number {
                            number: root.value("instancingDepth")
                            canBeNegative: false
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "Instanced Prims"
                        BriefNumber {
                            number: root.value("instancedRprims")
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "Bytes / Placement"
                        BriefNumber {
                            number: root.value("bytesPerPlacement")
                            memory: true
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "TLAS Build"
                        Duration {
                            seconds: root.value("tlasBuildMs") / 1000.0
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "TLAS Chunks"
                        Number {
                            number: root.value("tlasChunks")
                            canBeNegative: false
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                    SimpleTile {
                        label: "TLAS Scratch"
                        BriefNumber {
                            number: root.value("tlasScratchBytes")
                            memory: true
                            color: Theme.fg
                            size: Typography.large
                        }
                    }
                }
                SimpleTile {
                    id: largest
                    width: parent.width
                    label: `Most Placements (${root.value("largestPlacementCount")})`
                    Text {
                        width: largest.availableWidth
                        text: stats && stats.largestPlacementPrim ? stats.largestPlacementPrim : ""
                        elide: Text.ElideMiddle
                    }
                }
            }
        }

        // --- Where the device memory went ----------------------------------
        StatTable {
            width: parent.width
            label: "Scene Memory"
            interactive: true
            memory: true
            values: stats ? stats.deviceMemory : null
            grid.minValueWidthText: "999.9 MB"
        }

        // --- Where the time went -------------------------------------------
        //
        // Totals over the render, summed across Hydra's sync threads, so a
        // figure can exceed the elapsed time.
        ItemGrid {
            width: parent.width
            cellWidth: 110
            cellHeight: 40

            SimpleTile {
                label: "Ingest"
                Duration { seconds: root.value("ingestMs") / 1000.0; size: Typography.large }
            }
            SimpleTile {
                label: "Publish"
                Duration { seconds: root.value("publishMs") / 1000.0; size: Typography.large }
            }
            SimpleTile {
                label: "Instancing"
                Duration { seconds: root.value("instancingMs") / 1000.0; size: Typography.large }
            }
            SimpleTile {
                label: "Trace"
                Duration { seconds: root.value("traceMs") / 1000.0; size: Typography.large }
            }
            SimpleTile {
                label: "Materials"
                Duration { seconds: root.value("materialMs") / 1000.0; size: Typography.large }
            }
            SimpleTile {
                label: "Textures"
                Duration { seconds: root.value("textureMs") / 1000.0; size: Typography.large }
            }
        }
    }
}
