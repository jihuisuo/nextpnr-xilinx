/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  David Shah <dave@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "channel_router.h"
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <deque>
#include <fstream>
#include <queue>
#include <thread>
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace ChannelRouter {

inline bool operator==(const ChannelNode &a, const ChannelNode &b)
{
    return a.x == b.x && a.y == b.y && a.type == b.type;
}

inline bool operator!=(const ChannelNode &a, const ChannelNode &b)
{
    return a.x != b.x || a.y != b.y || a.type != b.type;
}

struct ChannelRouterState
{

    struct NodeScore
    {
        float cost;
        float togo_cost;
        delay_t delay;
        float total() const { return cost + togo_cost; }
    };

    struct PerNodeData
    {
        std::vector<ChannelNode> downhill;
        std::vector<ChannelNode> uphill;

        // net --> number of arcs; driving node
        boost::container::flat_map<int, std::pair<int, ChannelNode>> bound_nets;
        // Historical congestion cost
        float hist_cong_cost = 1.0;
        // Wire is unavailable as locked to another arc
        bool unavailable = false;
        // This wire has to be used for this net
        int reserved_net = -1;
        // Visit data
        struct
        {
            bool dirty = false, visited = false;
            ChannelNode bwd;
            NodeScore score;
        } visit;
    };

    struct PerArcData
    {
        ChannelNode sink_node;
        ArcBounds bb;
        bool routed = false;
    };

    // As we allow overlap at first; the nextpnr bind functions can't be used
    // as the primary relation between arcs and wires/pips
    struct PerNetData
    {
        ChannelNode src_node;
        std::vector<PerArcData> arcs;
        ArcBounds bb;
        // Coordinates of the center of the net, used for the weight-to-average
        int cx, cy, hpwl;
        int total_route_us = 0;
    };

    Context *ctx;
    ChannelRouterCfg cfg;
    ChannelGraph *g;
    int width, height;

    // xy -> index
    std::vector<std::vector<PerNodeData>> nodes;

    void setup_nodes()
    {
        width = g->get_width();
        height = g->get_height();
        nodes.resize(width * height);
        channel_types = g->get_channels();
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                auto &l = nodes.at(y * width + x);
                l.resize(channel_types.size());
            }
        }
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                for (int t = 0; t < int(channel_types.size()); t++) {
                    auto &c = channel_types.at(t);
                    for (const auto &dh : c.downhill) {
                        int start_x = x, start_y = y;
                        NPNR_ASSERT(dh.src_along <= c.length);
                        switch (c.dir) {
                        case DIR_EAST:
                            start_x -= dh.src_along;
                            break;
                        case DIR_WEST:
                            start_x += dh.src_along;
                            break;
                        case DIR_NORTH:
                            start_y -= dh.src_along;
                            break;
                        case DIR_SOUTH:
                            start_y += dh.src_along;
                            break;
                        }
                        int end_x = x, end_y = y;
                        auto &d = channel_types.at(dh.dst_type);
                        NPNR_ASSERT(dh.dst_along <= d.length);
                        switch (d.dir) {
                        case DIR_EAST:
                            end_x -= dh.dst_along;
                            break;
                        case DIR_WEST:
                            end_x += dh.dst_along;
                            break;
                        case DIR_NORTH:
                            end_y -= dh.dst_along;
                            break;
                        case DIR_SOUTH:
                            end_y += dh.dst_along;
                            break;
                        }
                        auto &src = nodes.at(start_y * width + start_x).at(t);
                        auto &dst = nodes.at(end_y * width + end_x).at(dh.dst_type);
                        src.downhill.emplace_back(end_x, end_y, dh.dst_type);
                        dst.uphill.emplace_back(start_x, start_y, t);
                    }
                }
            }
        }
    }

    std::vector<Channel> channel_types;

    // Use 'udata' for fast net lookups and indexing
    std::vector<NetInfo *> nets_by_udata;
    std::vector<PerNetData> nets;

    void setup_nets()
    {
        // Populate per-net and per-arc structures at start of routing
        nets.resize(ctx->nets.size());
        nets_by_udata.resize(ctx->nets.size());
        size_t i = 0;
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            ni->udata = i;
            nets_by_udata.at(i) = ni;
            nets.at(i).arcs.resize(ni->users.size());

            // Start net bounding box at overall min/max
            nets.at(i).bb.x0 = std::numeric_limits<int>::max();
            nets.at(i).bb.x1 = std::numeric_limits<int>::min();
            nets.at(i).bb.y0 = std::numeric_limits<int>::max();
            nets.at(i).bb.y1 = std::numeric_limits<int>::min();
            nets.at(i).cx = 0;
            nets.at(i).cy = 0;

            if (ni->driver.cell == nullptr) {
                nets.at(i).hpwl = 0;
                continue;
            }

            ChannelNode src_node = g->get_source_node(ni);
            nets.at(i).src_node = src_node;
            nets.at(i).cx += src_node.x;
            nets.at(i).cy += src_node.y;
            nets.at(i).bb.x0 = src_node.x;
            nets.at(i).bb.x1 = src_node.x;
            nets.at(i).bb.y0 = src_node.y;
            nets.at(i).bb.y1 = src_node.y;

            for (size_t j = 0; j < ni->users.size(); j++) {
                auto &usr = ni->users.at(j);

                ChannelNode sink_node = g->get_sink_node(ni, usr);
                nets.at(i).arcs.at(j).sink_node = sink_node;
                // Set bounding box for this arc
                nets.at(i).arcs.at(j).bb.x0 = std::min(src_node.x, sink_node.x);
                nets.at(i).arcs.at(j).bb.x1 = std::max(src_node.x, sink_node.x);
                nets.at(i).arcs.at(j).bb.y0 = std::min(src_node.y, sink_node.y);
                nets.at(i).arcs.at(j).bb.y1 = std::max(src_node.y, sink_node.y);
                // Expand net bounding box to include this arc
                nets.at(i).bb.x0 = std::min(nets.at(i).bb.x0, sink_node.x);
                nets.at(i).bb.x1 = std::max(nets.at(i).bb.x1, sink_node.x);
                nets.at(i).bb.y0 = std::min(nets.at(i).bb.y0, sink_node.y);
                nets.at(i).bb.y1 = std::max(nets.at(i).bb.y1, sink_node.y);
                // Add location to centroid sum
                nets.at(i).cx += sink_node.x;
                nets.at(i).cy += sink_node.y;
            }
            nets.at(i).hpwl = std::max(
                    std::abs(nets.at(i).bb.y1 - nets.at(i).bb.y0) + std::abs(nets.at(i).bb.x1 - nets.at(i).bb.x0), 1);
            nets.at(i).cx /= int(ni->users.size() + 1);
            nets.at(i).cy /= int(ni->users.size() + 1);
            if (ctx->debug)
                log_info("%s: bb=(%d, %d)->(%d, %d) c=(%d, %d) hpwl=%d\n", ctx->nameOf(ni), nets.at(i).bb.x0,
                         nets.at(i).bb.y0, nets.at(i).bb.x1, nets.at(i).bb.y1, nets.at(i).cx, nets.at(i).cy,
                         nets.at(i).hpwl);
            i++;
        }
    }

    struct QueuedNode
    {

        explicit QueuedNode(ChannelNode node, ChannelNode prev = ChannelNode(), NodeScore score = NodeScore{},
                            int randtag = 0)
                : node(node), prev(prev), score(score), randtag(randtag){};

        ChannelNode node;
        ChannelNode prev;
        NodeScore score;
        int randtag = 0;

        struct Greater
        {
            bool operator()(const QueuedNode &lhs, const QueuedNode &rhs) const noexcept
            {
                float lhs_score = lhs.score.cost + lhs.score.togo_cost,
                      rhs_score = rhs.score.cost + rhs.score.togo_cost;
                return lhs_score == rhs_score ? lhs.randtag > rhs.randtag : lhs_score > rhs_score;
            }
        };
    };

    double curr_cong_weight, hist_cong_weight, estimate_weight;

    PerNodeData &node_data(const ChannelNode &node)
    {
        NPNR_ASSERT(node.x >= 0 && node.x < width);
        NPNR_ASSERT(node.y >= 0 && node.y < height);
        return nodes.at(node.y * width + node.x).at(node.type);
    }

    float present_node_cost(const PerNodeData &w, int channel_type, int net_uid)
    {
        int over_capacity = int(w.bound_nets.size());
        over_capacity -= (channel_types.at(channel_type).width - 1);
        if (w.bound_nets.count(net_uid))
            over_capacity -= 1;
        if (over_capacity <= 0)
            return 1.0f;
        else
            return 1 + over_capacity * curr_cong_weight;
    }

    bool hit_test_node(ArcBounds &bb, ChannelNode n)
    {
        return n.x >= (bb.x0 - cfg.bb_margin_x) && n.x <= (bb.x1 + cfg.bb_margin_x) &&
               n.y >= (bb.y0 - cfg.bb_margin_y) && n.y <= (bb.y1 + cfg.bb_margin_y);
    }

    void bind_node_internal(NetInfo *net, ChannelNode node, ChannelNode uphill)
    {
        auto &b = node_data(node).bound_nets[net->udata];
        ++b.first;
        if (b.first == 1) {
            b.second = uphill;
        } else {
            NPNR_ASSERT(b.second == uphill);
        }
    }

    void unbind_node_internal(NetInfo *net, ChannelNode node)
    {
        auto &b = node_data(node).bound_nets.at(net->udata);
        --b.first;
        if (b.first == 0) {
            node_data(node).bound_nets.erase(net->udata);
        }
    }

    void ripup_arc(NetInfo *net, size_t user)
    {
        auto &ad = nets.at(net->udata).arcs.at(user);
        if (!ad.routed)
            return;
        ChannelNode src = nets.at(net->udata).src_node;
        ChannelNode cursor = ad.sink_node;
        while (cursor != src) {
            auto &wd = node_data(cursor);
            ChannelNode uphill = wd.bound_nets.at(net->udata).second;
            unbind_node_internal(net, cursor);
            cursor = uphill;
        }
        ad.routed = false;
    }

    float score_node_for_arc(NetInfo *net, size_t user, ChannelNode node)
    {
        auto &wd = node_data(node);
        auto &nd = nets.at(net->udata);
        float base_cost = channel_types.at(node.type).cost;
        float present_cost = present_node_cost(wd, node.type, net->udata);
        float hist_cost = wd.hist_cong_cost;
        float bias_cost = 0;
        int source_uses = 0;
        if (wd.bound_nets.count(net->udata))
            source_uses = wd.bound_nets.at(net->udata).first;
        bias_cost = cfg.bias_cost_factor * (base_cost / int(net->users.size())) *
                    ((std::abs(node.x - nd.cx) + std::abs(node.y - nd.cy)) / float(nd.hpwl));

        return base_cost * hist_cost * present_cost / (1 + source_uses) + bias_cost;
    }

    float get_togo_cost(NetInfo *net, size_t user, ChannelNode curr, ChannelNode sink)
    {
        auto &wd = node_data(curr);
        int source_uses = 0;
        if (wd.bound_nets.count(net->udata))
            source_uses = wd.bound_nets.at(net->udata).first;
        int base_cost =
                cfg.togo_cost_dx * abs(curr.x - sink.x) + cfg.togo_cost_dy * abs(curr.y - sink.y) + cfg.togo_cost_adder;
        return base_cost / (1 + source_uses);
    }
};
}; // namespace ChannelRouter

NEXTPNR_NAMESPACE_END
