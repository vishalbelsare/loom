// Copyright (c) 2014, Salesforce.com, Inc.  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// - Neither the name of Salesforce.com nor the names of its contributors
//   may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
// OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
// TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
// USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <loom/kind_kernel.hpp>
#include <loom/infer_grid.hpp>

namespace loom
{

KindKernel::KindKernel (
        const protobuf::Config::Kernels & config,
        const ProductValue & tare,
        CrossCat & cross_cat,
        Assignments & assignments,
        rng_t::result_type seed) :
    empty_group_count_(config.cat().empty_group_count()),
    empty_kind_count_(config.kind().empty_kind_count()),
    iterations_(config.kind().iterations()),
    score_parallel_(config.kind().score_parallel()),
    init_cache_(not config.hyper().run()),

    tare_(tare),
    cross_cat_(cross_cat),
    assignments_(assignments),
    kind_proposer_(),
    partial_values_(),
    rng_(seed),

    total_count_(0),
    change_count_(0),
    birth_count_(0),
    death_count_(0),
    tare_time_(0),
    score_time_(0),
    sample_time_(0),
    timer_()
{
    Timer::Scope timer(timer_);
    LOOM_ASSERT_LT(0, iterations_);
    LOOM_ASSERT_LT(0, empty_kind_count_);
    if (LOOM_DEBUG_LEVEL >= 1) {
        auto assigned_row_count = assignments_.row_count();
        auto cross_cat_row_count = cross_cat_.kinds[0].mixture.count_rows();
        LOOM_ASSERT_EQ(assigned_row_count, cross_cat_row_count);
    }

    init_featureless_kinds(empty_kind_count_);
    kind_proposer_.mixture_init_unobserved(cross_cat_, rng_);

    validate();
}

KindKernel::~KindKernel ()
{
    kind_proposer_.clear();
    init_featureless_kinds(0);

    validate();
}

bool KindKernel::try_run ()
{
    Timer::Scope timer(timer_);

    if (LOOM_DEBUG_LEVEL >= 1) {
        auto assigned_row_count = assignments_.row_count();
        auto cross_cat_row_count = cross_cat_.kinds[0].mixture.count_rows();
        auto proposer_row_count = kind_proposer_.kinds[0].mixture.count_rows();
        LOOM_ASSERT_EQ(assigned_row_count, cross_cat_row_count);
        LOOM_ASSERT_EQ(proposer_row_count, cross_cat_row_count);
    }

    validate();

    const auto old_kindids = cross_cat_.featureid_to_kindid;
    auto new_kindids = old_kindids;
    auto times = kind_proposer_.infer_assignments(
            tare_,
            cross_cat_,
            new_kindids,
            iterations_,
            score_parallel_,
            rng_);
    tare_time_ = times.tare;
    score_time_ = times.score;
    sample_time_ = times.sample;

    const size_t feature_count = old_kindids.size();
    size_t change_count = 0;
    for (size_t featureid = 0; featureid < feature_count; ++featureid) {
        size_t old_kindid = old_kindids[featureid];
        size_t new_kindid = new_kindids[featureid];
        if (new_kindid != old_kindid) {
            move_feature_to_kind(featureid, new_kindid);
            ++change_count;
        }
    }
    total_count_ = feature_count;
    change_count_ = change_count;

    size_t kind_count = cross_cat_.kinds.size();
    std::vector<size_t> kind_states(kind_count, 0);
    for (auto kindid : old_kindids) {
        kind_states[kindid] = 1;
    }
    for (auto kindid : new_kindids) {
        kind_states[kindid] |= 2;
    }
    size_t state_counts[4] = {0, 0, 0, 0};
    for (auto state : kind_states) {
        state_counts[state] += 1;
    }
    death_count_ = state_counts[1];
    birth_count_ = state_counts[2];

    init_featureless_kinds(empty_kind_count_);
    kind_proposer_.mixture_init_unobserved(cross_cat_, rng_);

    validate();

    return change_count > 0;
}

void KindKernel::add_featureless_kind ()
{
    auto & kind = cross_cat_.kinds.packed_add();
    auto & model = kind.model;
    auto & mixture = kind.mixture;
    model.clear();

    const auto & grid_prior = cross_cat_.hyper_prior.clustering();
    if (grid_prior.size()) {
        model.clustering = sample_clustering_prior(grid_prior, rng_);
    } else {
        model.clustering = cross_cat_.kinds[0].model.clustering;
    }

    const size_t row_count = assignments_.row_count();
    const std::vector<int> assignment_vector =
        model.clustering.sample_assignments(row_count, rng_);
    size_t group_count = 0;
    for (size_t groupid : assignment_vector) {
        group_count = std::max(group_count, 1 + groupid);
    }
    group_count += empty_group_count_;
    std::vector<int> counts(group_count, 0);
    auto & assignments = assignments_.packed_add();
    for (int groupid : assignment_vector) {
        assignments.push(groupid);
        ++counts[groupid];
    }
    mixture.init_unobserved(model, counts, rng_);
}

void KindKernel::remove_featureless_kind (size_t kindid)
{
    LOOM_ASSERT(
        cross_cat_.kinds[kindid].featureids.empty(),
        "cannot remove nonempty kind: " << kindid);

    cross_cat_.kinds.packed_remove(kindid);
    assignments_.packed_remove(kindid);

    // this is simpler than keeping a MixtureIdTracker for kinds
    if (kindid < cross_cat_.kinds.size()) {
        for (auto featureid : cross_cat_.kinds[kindid].featureids) {
            cross_cat_.featureid_to_kindid[featureid] = kindid;
        }
    }
}

void KindKernel::init_featureless_kinds (size_t featureless_kind_count)
{
    for (int i = cross_cat_.kinds.size() - 1; i >= 0; --i) {
        if (cross_cat_.kinds[i].featureids.empty()) {
            remove_featureless_kind(i);
        }
    }

    for (size_t i = 0; i < featureless_kind_count; ++i) {
        add_featureless_kind();
    }

    cross_cat_.update_splitter();

    cross_cat_.validate();
    assignments_.validate();
}

void KindKernel::move_feature_to_kind (
        size_t featureid,
        size_t new_kindid)
{
    size_t old_kindid = cross_cat_.featureid_to_kindid[featureid];
    LOOM_ASSERT_NE(new_kindid, old_kindid);

    CrossCat::Kind & old_kind = cross_cat_.kinds[old_kindid];
    CrossCat::Kind & new_kind = cross_cat_.kinds[new_kindid];
    KindProposer::Kind & proposed_kind = kind_proposer_.kinds[new_kindid];

    proposed_kind.mixture.move_feature_to(
        featureid,
        old_kind.model, old_kind.mixture,
        new_kind.model, new_kind.mixture,
        init_cache_,
        rng_);

    old_kind.featureids.erase(featureid);
    new_kind.featureids.insert(featureid);
    cross_cat_.featureid_to_kindid[featureid] = new_kindid;

    // TODO do this less frequently:
    cross_cat_.update_splitter();

    cross_cat_.validate();
    assignments_.validate();
}

} // namespace loom
