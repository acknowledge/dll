//=======================================================================
// Copyright (c) 2014-2016 Baptiste Wicht
// Distributed under the terms of the MIT License.
// (See accompanying file LICENSE or copy at
//  http://opensource.org/licenses/MIT)
//=======================================================================

/*!
 * \file
 * \brief Implementation of a Convolutional Restricted Boltzmann Machine
 */

#pragma once

#include <cstddef>
#include <ctime>
#include <random>

#include "cpp_utils/assert.hpp"     //Assertions
#include "cpp_utils/stop_watch.hpp" //Performance counter
#include "cpp_utils/maybe_parallel.hpp"
#include "cpp_utils/static_if.hpp"

#include "etl/etl.hpp"

#include "standard_conv_rbm.hpp" //The base class
#include "rbm_tmp.hpp"           // static_if macros

namespace dll {

/*!
 * \brief Convolutional Restricted Boltzmann Machine
 *
 * This follows the definition of a CRBM by Honglak Lee.
 */
template <typename Derived, typename Desc>
struct standard_crbm : public standard_conv_rbm<Derived, Desc> {
    using derived_t = Derived;
    using desc      = Desc;
    using weight    = typename desc::weight;
    using this_type = standard_crbm<derived_t, desc>;
    using base_type = standard_conv_rbm<Derived, desc>;

    using input_one_t  = typename rbm_base_traits<derived_t>::input_one_t;
    using output_one_t = typename rbm_base_traits<derived_t>::output_one_t;
    using input_t      = typename rbm_base_traits<derived_t>::input_t;
    using output_t     = typename rbm_base_traits<derived_t>::output_t;

    static constexpr const unit_type visible_unit = desc::visible_unit;
    static constexpr const unit_type hidden_unit  = desc::hidden_unit;

    standard_crbm() = default;

    // Make base class them participate in overload resolution
    using base_type::activate_hidden;
    using base_type::batch_activate_hidden;

    template <bool P = true, bool S = true, typename H1, typename H2, typename V1, typename V2>
    void activate_hidden(H1&& h_a, H2&& h_s, const V1& v_a, const V2& /*v_s*/) const {
        dll::auto_timer timer("crbm:activate_hidden");

        static_assert(hidden_unit == unit_type::BINARY || is_relu(hidden_unit), "Invalid hidden unit type");
        static_assert(P, "Computing S without P is not implemented");

        as_derived().template validate_inputs<V1, V2>();
        as_derived().template validate_outputs<H1, H2>();

        using namespace etl;

        auto b_rep = as_derived().get_b_rep();

        as_derived().reshape_h_a(h_a) = etl::conv_4d_valid_flipped(as_derived().reshape_v_a(v_a), as_derived().w);

        // Need to be done before h_a is computed!
        H_SAMPLE_PROBS(unit_type::RELU, f(h_s) = max(logistic_noise(b_rep + h_a), 0.0));
        H_SAMPLE_PROBS(unit_type::RELU6, f(h_s) = min(max(ranged_noise(b_rep + h_a, 6.0), 0.0), 6.0));
        H_SAMPLE_PROBS(unit_type::RELU1, f(h_s) = min(max(ranged_noise(b_rep + h_a, 1.0), 0.0), 1.0));

        H_PROBS2(unit_type::BINARY, unit_type::BINARY, f(h_a) = sigmoid(b_rep + h_a));
        H_PROBS2(unit_type::BINARY, unit_type::GAUSSIAN, f(h_a) = sigmoid((1.0 / (0.1 * 0.1)) >> (b_rep + h_a)));
        H_PROBS(unit_type::RELU, f(h_a) = max(b_rep + h_a, 0.0));
        H_PROBS(unit_type::RELU6, f(h_a) = min(max(b_rep + h_a, 0.0), 6.0));
        H_PROBS(unit_type::RELU1, f(h_a) = min(max(b_rep + h_a, 0.0), 1.0));

        H_SAMPLE_PROBS(unit_type::BINARY, f(h_s) = bernoulli(h_a));

        nan_check_deep(h_a);

        if (S) {
            nan_check_deep(h_s);
        }
    }

    void activate_hidden(output_one_t& h_a, const input_one_t& input) const {
        activate_hidden<true, false>(h_a, h_a, input, input);
    }

    template<typename Input>
    void activate_hidden(output_one_t& output, const Input& input) const {
        decltype(auto) converted = converter_one<Input, input_one_t>::convert(as_derived(), input);
        activate_hidden(output, converted);
    }

    template <bool P = true, bool S = true, typename H1, typename H2, typename V1, typename V2>
    void activate_visible(const H1& /*h_a*/, const H2& h_s, V1&& v_a, V2&& v_s) const {
        dll::auto_timer timer("crbm:activate_visible");

        static_assert(visible_unit == unit_type::BINARY || visible_unit == unit_type::GAUSSIAN, "Invalid visible unit type");
        static_assert(P, "Computing S without P is not implemented");

        as_derived().template validate_inputs<V1, V2>();
        as_derived().template validate_outputs<H1, H2>();

        using namespace etl;

        as_derived().reshape_v_a(v_a) = etl::conv_4d_full(as_derived().reshape_h_a(h_s), as_derived().w);

        auto c_rep = as_derived().get_c_rep();

        V_PROBS(unit_type::BINARY, f(v_a) = sigmoid(c_rep + v_a));
        V_PROBS(unit_type::GAUSSIAN, f(v_a) = c_rep + v_a);

        nan_check_deep(v_a);

        V_SAMPLE_PROBS(unit_type::BINARY, f(v_s) = bernoulli(v_a));
        V_SAMPLE_PROBS(unit_type::GAUSSIAN, f(v_s) = normal_noise(v_a));

        if (S) {
            nan_check_deep(v_s);
        }
    }

    template <bool P = true, bool S = true, typename H1, typename H2, typename V1, typename V2>
    void batch_activate_hidden(H1&& h_a, H2&& h_s, const V1& v_a, const V2& /*v_s*/) const {
        dll::auto_timer timer("crbm:batch_activate_hidden");

        static_assert(hidden_unit == unit_type::BINARY || is_relu(hidden_unit), "Invalid hidden unit type");
        static_assert(P, "Computing S without P is not implemented");

        as_derived().template validate_inputs<V1, V2, 1>();
        as_derived().template validate_outputs<H1, H2, 1>();

        using namespace etl;

        h_a = etl::conv_4d_valid_flipped(v_a, as_derived().w);

        auto b_rep = as_derived().get_batch_b_rep(v_a);

        // Need to be done before h_a is computed!
        H_SAMPLE_PROBS(unit_type::RELU, f(h_s) = max(logistic_noise(b_rep + h_a), 0.0));
        H_SAMPLE_PROBS(unit_type::RELU6, f(h_s) = min(max(ranged_noise(b_rep + h_a, 6.0), 0.0), 6.0));
        H_SAMPLE_PROBS(unit_type::RELU1, f(h_s) = min(max(ranged_noise(b_rep + h_a, 1.0), 0.0), 1.0));

        H_PROBS2(unit_type::BINARY, unit_type::BINARY, f(h_a) = sigmoid(b_rep + h_a));
        H_PROBS2(unit_type::BINARY, unit_type::GAUSSIAN, f(h_a) = sigmoid((1.0 / (0.1 * 0.1)) >> (b_rep + h_a)));
        H_PROBS(unit_type::RELU, f(h_a) = max(b_rep + h_a, 0.0));
        H_PROBS(unit_type::RELU6, f(h_a) = min(max(b_rep + h_a, 0.0), 6.0));
        H_PROBS(unit_type::RELU1, f(h_a) = min(max(b_rep + h_a, 0.0), 1.0));

        nan_check_deep(h_a);

        H_SAMPLE_PROBS(unit_type::BINARY, f(h_s) = bernoulli(h_a));

        if (S) {
            nan_check_deep(h_s);
        }
    }

    template <bool P = true, bool S = true, typename H1, typename H2, typename V1, typename V2>
    void batch_activate_visible(const H1& /*h_a*/, const H2& h_s, V1&& v_a, V2&& v_s) const {
        dll::auto_timer timer("crbm:batch_activate_visible");

        static_assert(visible_unit == unit_type::BINARY || visible_unit == unit_type::GAUSSIAN, "Invalid visible unit type");
        static_assert(P, "Computing S without P is not implemented");

        as_derived().template validate_inputs<V1, V2, 1>();
        as_derived().template validate_outputs<H1, H2, 1>();

        v_a = etl::conv_4d_full(h_s, as_derived().w);

        auto c_rep = as_derived().get_batch_c_rep(h_s);

        V_PROBS(unit_type::BINARY, f(v_a) = etl::sigmoid(c_rep + v_a));
        V_PROBS(unit_type::GAUSSIAN, f(v_a) = c_rep + v_a);

        nan_check_deep(v_a);

        V_SAMPLE_PROBS(unit_type::BINARY, f(v_s) = bernoulli(v_a));
        V_SAMPLE_PROBS(unit_type::GAUSSIAN, f(v_s) = normal_noise(v_a));

        if (S) {
            nan_check_deep(v_s);
        }
    }

    friend base_type;

private:

    weight energy_impl(const input_one_t& v, const output_one_t& h) const {
        auto tmp = as_derived().energy_tmp();
        tmp = etl::conv_4d_valid_flipped(as_derived().reshape_v_a(v), as_derived().w);

        if (desc::visible_unit == unit_type::BINARY && desc::hidden_unit == unit_type::BINARY) {
            //Definition according to Honglak Lee
            //E(v,h) = - sum_k hk . (Wk*v) - sum_k bk sum_h hk - c sum_v v

            return -etl::sum(as_derived().c >> etl::sum_r(v)) - etl::sum(as_derived().b >> etl::sum_r(h)) - etl::sum(h >> tmp(0));
        } else if (desc::visible_unit == unit_type::GAUSSIAN && desc::hidden_unit == unit_type::BINARY) {
            //Definition according to Honglak Lee / Mixed with Gaussian
            //E(v,h) = - sum_k hk . (Wk*v) - sum_k bk sum_h hk - sum_v ((v - c) ^ 2 / 2)

            auto c_rep = as_derived().get_c_rep();
            return -sum(etl::pow(v - c_rep, 2) / 2.0) - etl::sum(as_derived().b >> etl::sum_r(h)) - etl::sum(h >> tmp(0));
        } else {
            return 0.0;
        }
    }

    weight free_energy_impl(const input_one_t& v) const {
        auto tmp = as_derived().energy_tmp();
        tmp = etl::conv_4d_valid_flipped(as_derived().reshape_v_a(v), as_derived().w);

        if (desc::visible_unit == unit_type::BINARY && desc::hidden_unit == unit_type::BINARY) {
            //Definition computed from E(v,h)

            auto b_rep = as_derived().get_b_rep();
            auto x = b_rep + tmp(0);
            return -etl::sum(as_derived().c >> etl::sum_r(v)) - etl::sum(etl::log(1.0 + etl::exp(x)));
        } else if (desc::visible_unit == unit_type::GAUSSIAN && desc::hidden_unit == unit_type::BINARY) {
            //Definition computed from E(v,h)

            auto b_rep = as_derived().get_b_rep();
            auto x = b_rep + tmp(0);
            auto c_rep = as_derived().get_c_rep();
            return -sum(etl::pow(v - c_rep, 2) / 2.0) - etl::sum(etl::log(1.0 + etl::exp(x)));
        } else {
            return 0.0;
        }
    }

    derived_t& as_derived() {
        return *static_cast<derived_t*>(this);
    }

    const derived_t& as_derived() const {
        return *static_cast<const derived_t*>(this);
    }
};

} //end of dll namespace
