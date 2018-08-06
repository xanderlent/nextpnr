/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
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

#include "timing.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include "log.h"
#include "util.h"
#include <boost/range/adaptor/reversed.hpp>

NEXTPNR_NAMESPACE_BEGIN

typedef std::vector<const PortRef *> PortRefVector;
typedef std::map<int, unsigned> DelayFrequency;

struct Timing
{
    Context *ctx;
    bool net_delays;
    bool update;
    delay_t min_slack;
    PortRefVector current_path;
    PortRefVector *crit_path;
    DelayFrequency *slack_histogram;

    struct TimingData {
        TimingData() : max_arrival(), max_path_length(), min_remaining_budget() {}
        TimingData(delay_t max_arrival) : max_arrival(max_arrival), max_path_length(), min_remaining_budget() {}
        delay_t max_arrival;
        unsigned max_path_length = 0;
        delay_t min_remaining_budget;
    };

    Timing(Context *ctx, bool net_delays, bool update, PortRefVector *crit_path = nullptr,
           DelayFrequency *slack_histogram = nullptr)
            : ctx(ctx), net_delays(net_delays), update(update), min_slack(1.0e12 / ctx->target_freq),
              crit_path(crit_path), slack_histogram(slack_histogram)
    {
    }

    delay_t follow_net(NetInfo *net, int path_length, delay_t slack)
    {
        const delay_t default_budget = slack / (path_length + 1);
        delay_t net_budget = default_budget;
        for (auto &usr : net->users) {
            auto delay = net_delays ? ctx->getNetinfoRouteDelay(net, usr) : delay_t();
            if (crit_path)
                current_path.push_back(&usr);
            // If budget override exists, use that value and do not increment path_length
            auto budget = default_budget;
            if (ctx->getBudgetOverride(net, usr, budget)) {
                if (update)
                    usr.budget = std::min(usr.budget, budget);
                budget = follow_user_port(usr, path_length, slack - budget);
                net_budget = std::min(net_budget, budget);
            }
            else {
                budget = follow_user_port(usr, path_length + 1, slack - delay);
                net_budget = std::min(net_budget, budget);
                if (update)
                    usr.budget = std::min(usr.budget, delay + budget);
            }
            if (crit_path)
                current_path.pop_back();
        }
        return net_budget;
    }

    // Follow a path, returning budget to annotate
    delay_t follow_user_port(PortRef &user, int path_length, delay_t slack)
    {
        delay_t value;
        if (ctx->getPortClock(user.cell, user.port) != IdString()) {
            // At the end of a timing path (arguably, should check setup time
            // here too)
            value = slack / path_length;
            if (slack < min_slack) {
                min_slack = slack;
                if (crit_path)
                    *crit_path = current_path;
            }
            if (slack_histogram) {
                int slack_ps = ctx->getDelayNS(slack) * 1000;
                (*slack_histogram)[slack_ps]++;
            }
        } else {
            // Default to the path ending here, if no further paths found
            value = slack / path_length;
            // Follow outputs of the user
            for (auto port : user.cell->ports) {
                if (port.second.type == PORT_OUT) {
                    DelayInfo comb_delay;
                    // Look up delay through this path
                    bool is_path = ctx->getCellDelay(user.cell, user.port, port.first, comb_delay);
                    if (is_path) {
                        NetInfo *net = port.second.net;
                        if (net) {
                            delay_t path_budget = follow_net(net, path_length, slack - comb_delay.maxDelay());
                            value = std::min(value, path_budget);
                        }
                    }
                }
            }
        }
        return value;
    }

    delay_t walk_paths()
    {
        const auto clk_period = delay_t(1.0e12 / ctx->target_freq);

        // Go through all clocked drivers and distribute the available path
        //   slack evenly into the budget of every sink on the path
#if 0
        for (auto &cell : ctx->cells) {
            for (auto port : cell.second->ports) {
                if (port.second.type == PORT_OUT) {
                    IdString clock_domain = ctx->getPortClock(cell.second.get(), port.first);
                    if (clock_domain != IdString()) {
                        delay_t slack = clk_period; // TODO: clock constraints
                        DelayInfo clkToQ;
                        if (ctx->getCellDelay(cell.second.get(), clock_domain, port.first, clkToQ))
                            slack -= clkToQ.maxDelay();
                        if (port.second.net)
                            follow_net(port.second.net, 0, slack);
                    }
                }
            }
        }

#else
        std::vector<NetInfo*> topographical_order;
        std::unordered_map<const PortInfo*, unsigned> port_fanin;
        std::unordered_map<const NetInfo*, TimingData> net_data;

        std::vector<IdString> input_ports;
        std::vector<const PortInfo*> output_ports;
        for (auto &cell : ctx->cells) {
            input_ports.clear();
            output_ports.clear();
            bool is_io = cell.second->type == ctx->id_sb_io;
            for (auto& port : cell.second->ports) {
                if (!port.second.net) continue;
                if (port.second.type == PORT_OUT)
                    output_ports.push_back(&port.second);
                else
                    input_ports.push_back(port.first);
            }

            for (auto o : output_ports) {
                IdString clock_domain = ctx->getPortClock(cell.second.get(), o->name);
                if (clock_domain != IdString()) {
                    DelayInfo clkToQ;
                    ctx->getCellDelay(cell.second.get(), clock_domain, o->name, clkToQ);
                    topographical_order.emplace_back(o->net);
                    net_data.emplace(o->net, TimingData{ clkToQ.maxDelay() });
                }
                else {
                    if (is_io) {
                        topographical_order.emplace_back(o->net);
                        net_data.emplace(o->net, TimingData{});
                    }
                    for (auto i : input_ports) {
                        DelayInfo comb_delay;
                        bool is_path = ctx->getCellDelay(cell.second.get(), i, o->name, comb_delay);
                        if (is_path)
                            port_fanin[o]++;
                    }
                }
            }
        }

        std::deque<NetInfo*> queue(topographical_order.begin(), topographical_order.end());

        while (!queue.empty()) {
            const auto net = queue.front();
            queue.pop_front();

            for (auto &usr : net->users) {
                if (ctx->getPortClock(usr.cell, usr.port) != IdString()) {
                } else {
                    // Follow outputs of the user
                    for (auto& port : usr.cell->ports) {
                        if (port.second.type == PORT_OUT && port.second.net) {
                            DelayInfo comb_delay;
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (is_path) {
                                auto it = port_fanin.find(&port.second);
                                NPNR_ASSERT(it != port_fanin.end());
                                if (--it->second == 0) {
                                    topographical_order.emplace_back(port.second.net);
                                    queue.emplace_back(port.second.net);
                                    port_fanin.erase(it);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Find the maximum arrival time and max path length for each net
        for (auto net : topographical_order) {
            auto &nd = net_data.at(net);
            const auto net_arrival = nd.max_arrival;
            const auto net_length_plus_one = nd.max_path_length + 1;
            nd.min_remaining_budget = clk_period;
            for (auto &usr : net->users) {
                if (ctx->getPortClock(usr.cell, usr.port) != IdString()) {
                } else {
                    auto net_delay = ctx->getNetinfoRouteDelay(net, usr);
                    auto budget_override = ctx->getBudgetOverride(net, usr, net_delay);
                    auto usr_arrival = net_arrival + net_delay;
                    // Follow outputs of the user
                    for (auto port : usr.cell->ports) {
                        if (port.second.type == PORT_OUT && port.second.net) {
                            DelayInfo comb_delay;
                            // Look up delay through this path
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (is_path) {
                                auto& data = net_data[port.second.net];
                                auto& arrival = data.max_arrival;
                                arrival = std::max(arrival, usr_arrival + comb_delay.maxDelay());
                                if (!budget_override) {
                                    auto& path_length = data.max_path_length;
                                    path_length = std::max(path_length, net_length_plus_one);
                                }
                            }
                        }
                    }
                }
            }
        }

        for (auto net : boost::adaptors::reverse(topographical_order)) {
            auto &nd = net_data.at(net);
            const delay_t net_length_plus_one = nd.max_path_length + 1;
            auto& net_min_remaining_budget = nd.min_remaining_budget;
            for (auto &usr : net->users) {
                const auto net_delay = ctx->getNetinfoRouteDelay(net, usr);
                auto budget_override = ctx->getBudgetOverride(net, usr, net_delay);
                if (ctx->getPortClock(usr.cell, usr.port) != IdString()) {
                    const auto net_arrival = nd.max_arrival;
                    auto path_budget = clk_period - (net_arrival + net_delay);
                    auto budget_share = budget_override ? 0 : path_budget / net_length_plus_one;
                    usr.budget = std::min(usr.budget, net_delay + budget_share);
                    net_min_remaining_budget = std::min(net_min_remaining_budget, path_budget - budget_share);

                    min_slack = std::min(min_slack, path_budget);
                    if (slack_histogram) {
                        int slack_ps = ctx->getDelayNS(path_budget) * 1000;
                        (*slack_histogram)[slack_ps]++;
                    }
                } else {
                    // Follow outputs of the user
                    for (auto port : usr.cell->ports) {
                        if (port.second.type == PORT_OUT && port.second.net) {
                            DelayInfo comb_delay;
                            // Look up delay through this path
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (is_path) {
                                auto path_budget = net_data.at(port.second.net).min_remaining_budget;
                                auto budget_share = budget_override ? 0 : path_budget / net_length_plus_one;
                                usr.budget = std::min(usr.budget, net_delay + budget_share);
                                net_min_remaining_budget = std::min(net_min_remaining_budget, path_budget - budget_share);
                            }
                        }
                    }
                }
            }
        }
#endif
        return min_slack;
    }

    void assign_budget()
    {
        // Clear delays to a very high value first
        for (auto &net : ctx->nets) {
            for (auto &usr : net.second->users) {
                usr.budget = std::numeric_limits<delay_t>::max();
            }
        }

        walk_paths();
    }
};

void assign_budget(Context *ctx, bool quiet)
{
    if (!quiet) {
        log_break();
        log_info("Annotating ports with timing budgets for target frequency %.2f MHz\n", ctx->target_freq / 1e6);
    }

    Timing timing(ctx, ctx->slack_redist_iter > 0 /* net_delays */, true /* update */);
    timing.assign_budget();

    if (!quiet || ctx->verbose) {
        for (auto &net : ctx->nets) {
            for (auto &user : net.second->users) {
                // Post-update check
                if (!ctx->auto_freq && user.budget < 0)
                    log_warning("port %s.%s, connected to net '%s', has negative "
                                "timing budget of %fns\n",
                                user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                                ctx->getDelayNS(user.budget));
                else if (ctx->verbose)
                    log_info("port %s.%s, connected to net '%s', has "
                             "timing budget of %fns\n",
                             user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                             ctx->getDelayNS(user.budget));
            }
        }
    }

    // For slack redistribution, if user has not specified a frequency
    //   dynamically adjust the target frequency to be the currently
    //   achieved maximum
    if (ctx->auto_freq && ctx->slack_redist_iter > 0) {
        delay_t default_slack = delay_t(1.0e12 / ctx->target_freq);
        ctx->target_freq = 1e12 / (default_slack - timing.min_slack);
        if (ctx->verbose)
            log_info("minimum slack for this assign = %d, target Fmax for next "
                     "update = %.2f MHz\n",
                     timing.min_slack, ctx->target_freq / 1e6);
    }

    if (!quiet)
        log_info("Checksum: 0x%08x\n", ctx->checksum());
}

void timing_analysis(Context *ctx, bool print_histogram, bool print_path)
{
    PortRefVector crit_path;
    DelayFrequency slack_histogram;

    Timing timing(ctx, true /* net_delays */, false /* update */, print_path ? &crit_path : nullptr,
                  print_histogram ? &slack_histogram : nullptr);
    auto min_slack = timing.walk_paths();

    if (print_path) {
        if (crit_path.empty()) {
            log_info("Design contains no timing paths\n");
        } else {
            delay_t total = 0;
            log_break();
            log_info("Critical path report:\n");
            log_info("curr total\n");

            auto &front = crit_path.front();
            auto &front_port = front->cell->ports.at(front->port);
            auto &front_driver = front_port.net->driver;
            auto last_port = ctx->getPortClock(front_driver.cell, front_driver.port);
            for (auto sink : crit_path) {
                auto sink_cell = sink->cell;
                auto &port = sink_cell->ports.at(sink->port);
                auto net = port.net;
                auto &driver = net->driver;
                auto driver_cell = driver.cell;
                DelayInfo comb_delay;
                ctx->getCellDelay(sink_cell, last_port, driver.port, comb_delay);
                total += comb_delay.maxDelay();
                log_info("%4d %4d  Source %s.%s\n", comb_delay.maxDelay(), total, driver_cell->name.c_str(ctx),
                         driver.port.c_str(ctx));
                auto net_delay = ctx->getNetinfoRouteDelay(net, *sink);
                total += net_delay;
                auto driver_loc = ctx->getBelLocation(driver_cell->bel);
                auto sink_loc = ctx->getBelLocation(sink_cell->bel);
                log_info("%4d %4d    Net %s budget %d (%d,%d) -> (%d,%d)\n", net_delay, total, net->name.c_str(ctx),
                         sink->budget, driver_loc.x, driver_loc.y, sink_loc.x, sink_loc.y);
                log_info("                Sink %s.%s\n", sink_cell->name.c_str(ctx), sink->port.c_str(ctx));
                last_port = sink->port;
            }
            log_break();
        }
    }

    delay_t default_slack = delay_t(1.0e12 / ctx->target_freq);
    log_info("estimated Fmax = %.2f MHz\n", 1e6 / (default_slack - min_slack));

    if (print_histogram && slack_histogram.size() > 0) {
        constexpr unsigned num_bins = 20;
        unsigned bar_width = 60;
        auto min_slack = slack_histogram.begin()->first;
        auto max_slack = slack_histogram.rbegin()->first;
        auto bin_size = (max_slack - min_slack) / num_bins;
        std::vector<unsigned> bins(num_bins + 1);
        unsigned max_freq = 0;
        for (const auto &i : slack_histogram) {
            auto &bin = bins[(i.first - min_slack) / bin_size];
            bin += i.second;
            max_freq = std::max(max_freq, bin);
        }
        bar_width = std::min(bar_width, max_freq);

        log_break();
        log_info("Slack histogram:\n");
        log_info(" legend: * represents %d endpoint(s)\n", max_freq / bar_width);
        for (unsigned i = 0; i < bins.size(); ++i)
            log_info("%6d < ps < %6d |%s\n", min_slack + bin_size * i, min_slack + bin_size * (i + 1),
                     std::string(bins[i] * bar_width / max_freq, '*').c_str());
    }
}

NEXTPNR_NAMESPACE_END
