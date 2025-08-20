// Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// with the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// * Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
//
// * Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimers in the
// documentation and/or other materials provided with the distribution.
//
// * Neither the names of Advanced Micro Devices, Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this Software without specific prior written permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.

#include "core/agent.hpp"
#include "core/trace_cache/cache_manager.hpp"
#include "core/trace_cache/cache_utility.hpp"
#include "core/trace_cache/sample_type.hpp"
#include <amd_smi/amdsmi.h>
#include <limits>
#if defined(NDEBUG)
#    undef NDEBUG
#endif

#include "core/agent_manager.hpp"
#include "core/common.hpp"
#include "core/components/fwd.hpp"
#include "core/config.hpp"
#include "core/debug.hpp"
#include "core/gpu.hpp"
#include "core/node_info.hpp"
#include "core/perfetto.hpp"
#include "core/rocpd/data_processor.hpp"
#include "core/state.hpp"
#include "core/trace_cache/metadata_registry.hpp"
#include "library/amd_smi.hpp"
#include "library/runtime.hpp"
#include "library/thread_info.hpp"

#include <timemory/backends/threading.hpp>
#include <timemory/components/timing/backends.hpp>
#include <timemory/mpl/type_traits.hpp>
#include <timemory/units.hpp>
#include <timemory/utility/delimit.hpp>
#include <timemory/utility/locking.hpp>

#include <cassert>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <tuple>

#define ROCPROFSYS_AMD_SMI_CALL(...)                                                     \
    ::rocprofsys::amd_smi::check_error(__FILE__, __LINE__, __VA_ARGS__)

namespace rocprofsys
{
namespace amd_smi
{
using bundle_t          = std::deque<data>;
using sampler_instances = thread_data<bundle_t, category::amd_smi>;

namespace
{
enum class metric_type : uint8_t
{
    busy = 0,
    temp,
    power,
    mem_usage,
    vcn_activity,
    jpeg_activity,
    invalid
};

metric_type
metric_type_from_string(const std::string& str)
{
#define FROM_STRING(ENUM)                                                                \
    if(str == #ENUM) return metric_type::ENUM;
    FROM_STRING(busy);
    FROM_STRING(temp);
    FROM_STRING(power);
    FROM_STRING(mem_usage);
    FROM_STRING(vcn_activity);
    FROM_STRING(jpeg_activity);
#undef FROM_STRING
    return metric_type::invalid;
};

std::string
metric_type_to_string(metric_type type)
{
#define TO_STRING_CASE(ENUM)                                                             \
    case metric_type::ENUM: return #ENUM;
    switch(type)
    {
        TO_STRING_CASE(busy);
        TO_STRING_CASE(temp);
        TO_STRING_CASE(power);
        TO_STRING_CASE(mem_usage);
        TO_STRING_CASE(vcn_activity);
        TO_STRING_CASE(jpeg_activity);
        default: return "";
    }
#undef FROM_STRING
};

// remove
rocpd::data_processor&
get_data_processor()
{
    return rocpd::data_processor::get_instance();
}

void
metadata_initialize_category()
{
    trace_cache::get_metadata_registry().add_string(
        trait::name<category::amd_smi>::value);
}

void
metadata_initialize_smi_tracks()
{
    const auto thread_id = std::nullopt;

    trace_cache::get_metadata_registry().add_track(
        { trait::name<category::amd_smi_mm_busy>::value, thread_id, "{}" });
    trace_cache::get_metadata_registry().add_track(
        { trait::name<category::amd_smi_power>::value, thread_id, "{}" });
    trace_cache::get_metadata_registry().add_track(
        { trait::name<category::amd_smi_temp>::value, thread_id, "{}" });
    trace_cache::get_metadata_registry().add_track(
        { trait::name<category::amd_smi_memory_usage>::value, thread_id, "{}" });
}

void
metadata_initialize_smi_pmc(size_t gpu_id)
{
    // TODO: Find the proper values for a following definitions
    size_t      EVENT_CODE       = 0;
    size_t      INSTANCE_ID      = 0;
    const char* LONG_DESCRIPTION = "";
    const char* COMPONENT        = "";
    const char* BLOCK            = "";
    const char* EXPRESSION       = "";
    const char* CELSIUS_DEGREES  = "\u00B0C";
    auto        ni               = node_info::get_instance();
    const char* TARGET_ARCH      = "GPU";

    trace_cache::get_metadata_registry().add_pmc_info(
        { agent_type::GPU, gpu_id, TARGET_ARCH, EVENT_CODE, INSTANCE_ID,
          trait::name<category::amd_smi_mm_busy>::value, "Busy",
          trait::name<category::amd_smi_mm_busy>::description, LONG_DESCRIPTION,
          COMPONENT, trace_cache::PERCENTAGE, rocprofsys::trace_cache::ABSOLUTE, BLOCK,
          EXPRESSION, 0, 0, "{}" });

    trace_cache::get_metadata_registry().add_pmc_info(
        { agent_type::GPU, gpu_id, TARGET_ARCH, EVENT_CODE, INSTANCE_ID,
          trait::name<category::amd_smi_temp>::value, "Temp",
          trait::name<category::amd_smi_temp>::description, LONG_DESCRIPTION, COMPONENT,
          CELSIUS_DEGREES, rocprofsys::trace_cache::ABSOLUTE, BLOCK, EXPRESSION, 0, 0 });

    trace_cache::get_metadata_registry().add_pmc_info(
        { agent_type::GPU, gpu_id, TARGET_ARCH, EVENT_CODE, INSTANCE_ID,
          trait::name<category::amd_smi_power>::value, "Pow",
          trait::name<category::amd_smi_power>::description, LONG_DESCRIPTION, COMPONENT,
          "W", rocprofsys::trace_cache::ABSOLUTE, BLOCK, EXPRESSION, 0, 0 });

    trace_cache::get_metadata_registry().add_pmc_info(
        { agent_type::GPU, gpu_id, TARGET_ARCH, EVENT_CODE, INSTANCE_ID,
          trait::name<category::amd_smi_memory_usage>::value, "MemUsg",
          trait::name<category::amd_smi_memory_usage>::description, LONG_DESCRIPTION,
          COMPONENT, tim::units::mem_repr(tim::units::megabyte),
          rocprofsys::trace_cache::ABSOLUTE, BLOCK, EXPRESSION, 0, 0 });
}

// move to rocpd post processing
void
rocpd_process_smi_pmc_events(const uint32_t device_id, const amd_smi::settings& settings,
                             uint64_t timestamp, double busy, double temp, double power,
                             double usage)
{
    if(!(settings.busy || settings.temp || settings.power || settings.mem_usage)) return;

    auto& data_processor = get_data_processor();

    const auto* _name            = trait::name<category::amd_smi>::value;
    auto        name_primary_key = data_processor.insert_string(_name);
    auto        event_id         = data_processor.insert_event(name_primary_key, 0, 0, 0);

    auto& _agent_manager = agent_manager::get_instance();
    auto  base_id =
        _agent_manager.get_agent_by_type_index(device_id, agent_type::GPU).base_id;

    auto insert_event_and_sample = [&](bool enabled, const char* name, double value) {
        if(!enabled) return;
        data_processor.insert_pmc_event(event_id, base_id, name, value);
        data_processor.insert_sample(name, timestamp, event_id);
    };

    insert_event_and_sample(settings.busy, trait::name<category::amd_smi_mm_busy>::value,
                            busy);
    insert_event_and_sample(settings.temp, trait::name<category::amd_smi_temp>::value,
                            temp);
    insert_event_and_sample(settings.power, trait::name<category::amd_smi_power>::value,
                            power);
    insert_event_and_sample(settings.mem_usage,
                            trait::name<category::amd_smi_memory_usage>::value, usage);
}

// move map to class
auto&
get_settings(uint32_t _dev_id)
{
    static auto _v = std::unordered_map<uint32_t, amd_smi::settings>{};
    return _v[_dev_id];
}

// ?
bool&
is_initialized()
{
    static bool _v = false;
    return _v;
}
// helper
amdsmi_version_t&
get_version()
{
    static amdsmi_version_t _v = {};

    if(_v.major == 0 && _v.minor == 0)
    {
        auto _err = amdsmi_get_lib_version(&_v);
        if(_err != AMDSMI_STATUS_SUCCESS)
            ROCPROFSYS_THROW(
                "amdsmi_get_version failed. No version information available.");
    }

    return _v;
}

// change signature, remove _option, write it simpler
void
check_error(const char* _file, int _line, amdsmi_status_t _code, bool* _option = nullptr)
{
    if(_code == AMDSMI_STATUS_SUCCESS)
        return;
    else if(_code == AMDSMI_STATUS_NOT_SUPPORTED && _option)
    {
        *_option = false;
        return;
    }

    const char* _msg = nullptr;
    auto        _err = amdsmi_status_code_to_string(_code, &_msg);
    if(_err != AMDSMI_STATUS_SUCCESS)
        ROCPROFSYS_THROW(
            "amdsmi_status_code_to_string failed. No error message available. "
            "Error code %i originated at %s:%i\n",
            static_cast<int>(_code), _file, _line);
    ROCPROFSYS_THROW("[%s:%i] Error code %i :: %s", _file, _line, static_cast<int>(_code),
                     _msg);
}

bool
check_success(amdsmi_status_t _code)
{
    return _code == AMDSMI_STATUS_SUCCESS;
}

bool
check_if_is_supported(amdsmi_status_t _code)
{
    return _code != AMDSMI_STATUS_NOT_SUPPORTED;
}

// move to class
std::atomic<State>&
get_state()
{
    static std::atomic<State> _v{ State::PreInit };
    return _v;
}
}  // namespace

//--------------------------------------------------------------------------------------//

class amd_smi_sampler
{
public:
    struct xcp_metrics_t
    {
        std::vector<uint16_t> vcn_busy;
        std::vector<uint16_t> jpeg_busy;
    };

    // gpu::device_count(), config::get_sampling_gpus(),
    // get_setting_value<std::string>("ROCPROFSYS_AMD_SMI_METRICS");
    amd_smi_sampler(int device_count, const std::string& sampling_gpus,
                    const std::string& _metrics)
    {
        auto_lock_t _lk{ type_mutex<category::amd_smi>() };

        if(is_initialized() || !get_use_amd_smi()) return;  // prevent this

        ROCPROFSYS_SCOPED_SAMPLING_ON_CHILD_THREADS(false);

        if(!gpu::initialize_amdsmi())
        {
            ROCPROFSYS_WARNING_F(
                0, "AMD SMI is not available. Disabling AMD SMI sampling...");  // prevent
                                                                                // this
            return;
        }

        amdsmi_version_t _version = get_version();
        ROCPROFSYS_VERBOSE_F(0, "AMD SMI version: %u.%u.%u - str: %s.\n", _version.major,
                             _version.minor, _version.release, _version.build);

        configure_devices(device_count, sampling_gpus);
        configure_metrics(_metrics);

        is_initialized() = true;  // ?? Prevent this
        perfetto_counter_track<data>::init();
        amd_smi::set_state(State::PreInit);
    }

    ~amd_smi_sampler()
    {
        auto_lock_t _lk{ type_mutex<category::amd_smi>() };
        // if(!is_initialized()) return;
        ROCPROFSYS_VERBOSE_F(1, "Shutting down amd-smi...\n");
        amd_smi::set_state(State::Finalized);
        bool is_enabled = true;
        amdsmi_get(is_enabled, amdsmi_shut_down);
        ROCPROFSYS_AMD_SMI_CALL(amdsmi_shut_down());
        // is_initialized() = false;
    }

    void sample(uint32_t _device_id)
    {
        if(is_child_process()) return;

        auto _timestamp = tim::get_clock_real_now<size_t, std::nano>();
        assert(_timestamp < std::numeric_limits<int64_t>::max());

        auto _state = get_state().load();

        if(_state != State::Active) return;

        bool                    _vcn_or_jpeg_activity_enabled = false;
        amdsmi_processor_handle sample_handle = gpu::get_handle_from_id(_device_id);

        amdsmi_engine_usage_t      busy_perc;
        int64_t                    temperature;
        amdsmi_power_info_t        power;
        uint64_t                   mem_usage;
        amdsmi_gpu_metrics_t       _gpu_metrics;
        std::vector<xcp_metrics_t> xcp_metrics = {};

        amdsmi_get(m_settings[_device_id][metric_type::busy], amdsmi_get_gpu_activity,
                   sample_handle, &busy_perc);

        amdsmi_get(m_settings[_device_id][metric_type::temp], amdsmi_get_temp_metric,
                   sample_handle, AMDSMI_TEMPERATURE_TYPE_JUNCTION, AMDSMI_TEMP_CURRENT,
                   &temperature);
#if(AMDSMI_LIB_VERSION_MAJOR == 2 && AMDSMI_LIB_VERSION_MINOR == 0) ||                   \
    (AMDSMI_LIB_VERSION_MAJOR == 25 && AMDSMI_LIB_VERSION_MINOR == 2)
        // This was a transient change in the AMD SMI API. It was never officially
        // released.
        amdsmi_get(m_settings[_device_id][metric_type::power], amdsmi_get_power_info,
                   sample_handle, 0, &m_power)
#else
        amdsmi_get(m_settings[_device_id][metric_type::power], amdsmi_get_power_info,
                   sample_handle, &power);
#endif
            amdsmi_get(m_settings[_device_id][metric_type::mem_usage],
                       amdsmi_get_gpu_memory_usage, sample_handle, AMDSMI_MEM_TYPE_VRAM,
                       &mem_usage);

        _vcn_or_jpeg_activity_enabled =
            m_settings[_device_id][metric_type::vcn_activity] ||
            m_settings[_device_id][metric_type::jpeg_activity];
        if(!_vcn_or_jpeg_activity_enabled)
        {
            amdsmi_get(_vcn_or_jpeg_activity_enabled, amdsmi_get_gpu_metrics_info,
                       sample_handle, &_gpu_metrics);

            if(_vcn_or_jpeg_activity_enabled)
            {
                process_xcp_metrics(_device_id, _gpu_metrics, xcp_metrics);
            }
            else
            {
                // if not supported, disable both
                m_settings[_device_id][metric_type::vcn_activity]  = false;
                m_settings[_device_id][metric_type::jpeg_activity] = false;
            }
        }

        auto xcp_metrics_serialized = serialize_xcp_metrics(xcp_metrics);

        // TODO: Should we switch to multiple stores to filter out only enabled metrics?
        // Child processes will not sample AMD-SMI
        // Only root process will do the sampling

        trace_cache::get_buffer_storage().store(
            trace_cache::entry_type::amd_smi_sample,
            static_cast<uint8_t>(trace_cache::amd_smi_sample::type::gfx_activity),
            _device_id, _timestamp, busy_perc.gfx_activity);
        trace_cache::get_buffer_storage().store(
            trace_cache::entry_type::amd_smi_sample,
            static_cast<uint8_t>(trace_cache::amd_smi_sample::type::gfx_activity),
            _device_id, _timestamp, busy_perc.umc_activity);
        trace_cache::get_buffer_storage().store(
            trace_cache::entry_type::amd_smi_sample,
            static_cast<uint8_t>(trace_cache::amd_smi_sample::type::gfx_activity),
            _device_id, _timestamp, busy_perc.mm_activity);
        trace_cache::get_buffer_storage().store(
            trace_cache::entry_type::amd_smi_sample,
            static_cast<uint8_t>(trace_cache::amd_smi_sample::type::gfx_activity),
            _device_id, _timestamp, temperature);
             power.current_socket_power,
            mem_usage,
             xcp_metrics_serialized);
    }

private:
    enum class Filter : uint8_t
    {
        All,
        None,
        Specific
    };

    std::vector<uint8_t> serialize_xcp_metrics(
        const std::vector<amd_smi_sampler::xcp_metrics_t>& metrics_vec)
    {
        size_t total_size = sizeof(size_t);  // for root vector size
        for(const auto& metrics : metrics_vec)
        {
            total_size += sizeof(size_t) + metrics.vcn_busy.size() * sizeof(uint16_t);
            total_size += sizeof(size_t) + metrics.jpeg_busy.size() * sizeof(uint16_t);
        }

        std::vector<uint8_t> buffer;
        buffer.reserve(total_size);

        size_t offset = 0;
        auto   append = [&](auto value) {
            size_t sz = sizeof(value);
            std::memcpy(buffer.data() + offset, &value, sz);
            offset += sz;
        };

        append(static_cast<size_t>(metrics_vec.size()));

        for(const auto& metrics : metrics_vec)
        {
            append(static_cast<size_t>(metrics.vcn_busy.size()));
            for(uint16_t v : metrics.vcn_busy)
                append(v);

            append(static_cast<size_t>(metrics.jpeg_busy.size()));
            for(uint16_t j : metrics.jpeg_busy)
                append(j);
        }

        return buffer;
    }

    void configure_devices(size_t _device_count, const std::string& _sampling_gpus)
    {
        std::string _devices_v;
        std::transform(_sampling_gpus.begin(), _sampling_gpus.end(), _devices_v.begin(),
                       [](const auto& c) { return std::tolower(c); });

        if(_devices_v == "off")
            _devices_v = "none";
        else if(_devices_v == "on")
            _devices_v = "all";

        Filter filter = Filter::Specific;
        if(_devices_v == "all" || _devices_v.empty())
        {
            filter = Filter::All;
        }
        if(_devices_v == "none")
        {
            filter = Filter::None;
        }

        std::set<uint32_t> _devices = {};
        auto _emplace = [_device_count, &_devices](const uint32_t& device_id) {
            if(device_id < _device_count) _devices.emplace(device_id);
        };

        if(filter == Filter::All)
        {
            for(uint32_t i = 0; i < _device_count; ++i)
                _emplace(i);
        }
        else if(filter == Filter::Specific)
        {
            auto _enabled = tim::delimit(_devices_v, ",; \t");

            for(auto&& itr : _enabled)
            {
                auto type = parse_specification(itr);
                if(type == SpecificationType::Range_Entry)
                {
                    auto _range       = tim::delimit(itr, "-");
                    auto [begin, end] = parse_range_format(itr, _range);
                    for(auto i = begin; i < end; ++i)
                        _emplace(i);
                }
                else
                {
                    _emplace(std::stoul(itr));
                }
            }
        }

        m_device_count = _device_count;  // Doesn't make sense to keep device count like
                                         // this if we filter devices
        m_device_list = _devices;
    }

    enum class SpecificationType : uint8_t
    {
        Single_Entry,
        Range_Entry,
    };

    static SpecificationType parse_specification(const std::string& _specification)
    {
        auto is_valid =
            _specification.find_first_not_of("0123456789-") != std::string::npos;
        if(!is_valid)
        {
            ROCPROFSYS_THROW("Invalid GPU specification: '%s'. Only numerical values "
                             "(e.g., 0) or ranges (e.g., 0-7) are permitted.",
                             _specification.c_str());
        }

        return _specification.find('-') != std::string::npos
                   ? SpecificationType::Range_Entry
                   : SpecificationType::Single_Entry;
    };

    static std::tuple<size_t, size_t> parse_range_format(
        const std::string&              _range_specification,
        const std::vector<std::string>& _range_delimited)
    {
        ROCPROFSYS_CONDITIONAL_THROW(_range_delimited.size() != 2,
                                     "Invalid GPU range specification: '%s'. "
                                     "Required format N-M, e.g. 0-4",
                                     _range_specification.c_str());

        auto begin = std::stoul(_range_delimited.at(0));
        auto end   = std::stoul(_range_delimited.at(1));
        ROCPROFSYS_CONDITIONAL_THROW(
            end >= begin,
            "Invalid GPU range specification: '%s'. "
            "Required end of ragne to be greater than begin, e.g. 1-5",
            _range_specification.c_str());
        return std::make_tuple(begin, end);
    };

    void initialize_supported()
    {
        for(const auto& device_id : m_device_list)
        {
            sample(device_id);
        }
    }

    void disable_all_settings(uint32_t _device_id)
    {
        m_settings[_device_id][metric_type::busy]          = false;
        m_settings[_device_id][metric_type::temp]          = false;
        m_settings[_device_id][metric_type::power]         = false;
        m_settings[_device_id][metric_type::mem_usage]     = false;
        m_settings[_device_id][metric_type::vcn_activity]  = false;
        m_settings[_device_id][metric_type::jpeg_activity] = false;
    }

    void configure_metrics(const std::optional<std::string>& _metrics)
    {
        initialize_supported();
        if(!_metrics.has_value() || _metrics->empty())
        {
            return;
        }

        if(*_metrics == "all")
        {
            // All supported samples are already enabled
            return;
        }
        if(*_metrics == "none")
        {
            for(const auto& _device_id : m_device_list)
            {
                disable_all_settings(_device_id);
            }
            return;
        }

        auto                     metrics = tim::delimit(*_metrics, ",;:\t\n ");
        std::vector<metric_type> required_metrics;
        required_metrics.reserve(metrics.size());
        for(const auto& metric : metrics)
        {
            required_metrics.push_back(metric_type_from_string(metric));
        }

        if(std::count(required_metrics.begin(), required_metrics.end(),
                      metric_type::invalid) > 0)
        {
            ROCPROFSYS_WARNING(
                0, "Unsupported amd-smi metric detected. Ignoring invalid input.");

            // Prevent invalid metrics
            required_metrics.erase(std::remove(required_metrics.begin(),
                                               required_metrics.end(),
                                               [](const metric_type& type) {
                                                   return type == metric_type::invalid;
                                               }),
                                   required_metrics.end());
        }

        for(auto _device_id : m_device_list)
        {
            auto supported = m_settings[_device_id];

            disable_all_settings(_device_id);

            for(const auto& metric : required_metrics)
            {
                auto iitr = supported.find(metric);

                ROCPROFSYS_VERBOSE_F(1, "Enabling amd-smi metric '%s' on device [%u]\n",
                                     metric_type_to_string(metric).c_str(), _device_id);

                iitr->second = true;
            }

            m_settings[_device_id] = supported;
        }
    }

    template <typename T>
    static std::vector<uint16_t> extract_valid_metrics(const T& source_array)
    {
        std::vector<uint16_t> result;
        for(const auto& val : source_array)
        {
            if(val != std::numeric_limits<uint16_t>::max())
            {
                result.push_back(val);
            }
        }
        return result;
    }

    static xcp_metrics_t create_xcp_metrics(const std::vector<uint16_t>& vcn_data,
                                            const std::vector<uint16_t>& jpeg_data)
    {
        xcp_metrics_t metrics;
        metrics.vcn_busy  = vcn_data;
        metrics.jpeg_busy = jpeg_data;
        return metrics;
    }

    static bool has_metrics_data(const xcp_metrics_t& metrics)
    {
        return !metrics.vcn_busy.empty() || !metrics.jpeg_busy.empty();
    }

    static void process_xcp_metrics(uint32_t                    device_id,
                                    const amdsmi_gpu_metrics_t& gpu_metrics,
                                    std::vector<xcp_metrics_t>& xcp_metrics)
    {
        const bool vcn_supported  = gpu::is_vcn_activity_supported(device_id);
        const bool jpeg_supported = gpu::is_jpeg_activity_supported(device_id);

        if(vcn_supported || jpeg_supported)
        {
            auto vcn_data  = vcn_supported
                                 ? extract_valid_metrics(gpu_metrics.vcn_activity)
                                 : std::vector<uint16_t>{};
            auto jpeg_data = jpeg_supported
                                 ? extract_valid_metrics(gpu_metrics.jpeg_activity)
                                 : std::vector<uint16_t>{};

            auto metrics = create_xcp_metrics(vcn_data, jpeg_data);
            if(has_metrics_data(metrics))
            {
                xcp_metrics.push_back(metrics);
            }
            return;
        }

        for(const auto& xcp : gpu_metrics.xcp_stats)
        {
            auto vcn_data  = extract_valid_metrics(xcp.vcn_busy);
            auto jpeg_data = extract_valid_metrics(xcp.jpeg_busy);

            auto metrics = create_xcp_metrics(vcn_data, jpeg_data);
            if(has_metrics_data(metrics))
            {
                xcp_metrics.push_back(metrics);
            }
        }
    }

    template <typename GetterFunction, typename... Args,
              typename = std::enable_if_t<std::is_same_v<
                  std::invoke_result_t<std::decay_t<GetterFunction>, Args...>,
                  amdsmi_status_t>>>
    void amdsmi_get(bool& option, GetterFunction&& func, Args&&... args)
    {
        if(option)
        {
            auto _code = std::invoke(std::forward<GetterFunction>(func),
                                     std::forward<Args>(args)...);
            if(::rocprofsys::amd_smi::check_success(_code))
            {
                return;
            }

            if(!::rocprofsys::amd_smi::check_if_is_supported(_code))
            {
                option = false;
                return;
            }

            // TODO: Log better message
            ROCPROFSYS_VERBOSE_F(0, "Disabling future samples from amd-smi...\n");
            amd_smi::set_state(State::Disabled);
        }
    }

private:
    using device_id_t    = uint32_t;
    using metric_enabled = std::unordered_map<metric_type, bool>;

    size_t                                          m_device_count{ 0 };
    std::set<device_id_t>                           m_device_list{};
    std::unordered_map<device_id_t, metric_enabled> m_settings{};
};

// move to class

// sample should be just method
data::data(uint32_t _dev_id) { sample(_dev_id); }

void
data::sample(uint32_t _dev_id)
{
    if(is_child_process()) return;

    auto _ts = tim::get_clock_real_now<size_t, std::nano>();
    assert(_ts < std::numeric_limits<int64_t>::max());
    amdsmi_gpu_metrics_t _gpu_metrics;
    bool                 _vcn_or_jpeg_activity_enabled = false;

    auto _state = get_state().load();

    if(_state != State::Active) return;

    m_dev_id = _dev_id;
    m_ts     = _ts;

    // move to function - start
#define ROCPROFSYS_AMDSMI_GET(OPTION, FUNCTION, ...)                                     \
    if(OPTION)                                                                           \
    {                                                                                    \
        try                                                                              \
        {                                                                                \
            ROCPROFSYS_AMD_SMI_CALL(FUNCTION(__VA_ARGS__), &OPTION);                     \
        } catch(std::runtime_error & _e)                                                 \
        {                                                                                \
            ROCPROFSYS_VERBOSE_F(                                                        \
                0, "[%s] Exception: %s. Disabling future samples from amd-smi...\n",     \
                #FUNCTION, _e.what());                                                   \
            get_state().store(State::Disabled);                                          \
        }                                                                                \
    }

    amdsmi_processor_handle sample_handle = gpu::get_handle_from_id(_dev_id);
    ROCPROFSYS_AMDSMI_GET(get_settings(m_dev_id).busy, amdsmi_get_gpu_activity,
                          sample_handle, &m_busy_perc);
    ROCPROFSYS_AMDSMI_GET(get_settings(m_dev_id).temp, amdsmi_get_temp_metric,
                          sample_handle, AMDSMI_TEMPERATURE_TYPE_JUNCTION,
                          AMDSMI_TEMP_CURRENT, &m_temp);
#if(AMDSMI_LIB_VERSION_MAJOR == 2 && AMDSMI_LIB_VERSION_MINOR == 0) ||                   \
    (AMDSMI_LIB_VERSION_MAJOR == 25 && AMDSMI_LIB_VERSION_MINOR == 2)
    // This was a transient change in the AMD SMI API. It was never officially released.
    ROCPROFSYS_AMDSMI_GET(get_settings(m_dev_id).power, amdsmi_get_power_info,
                          sample_handle, 0, &m_power)
#else
    ROCPROFSYS_AMDSMI_GET(get_settings(m_dev_id).power, amdsmi_get_power_info,
                          sample_handle, &m_power)
#endif
    ROCPROFSYS_AMDSMI_GET(get_settings(m_dev_id).mem_usage, amdsmi_get_gpu_memory_usage,
                          sample_handle, AMDSMI_MEM_TYPE_VRAM, &m_mem_usage);
    _vcn_or_jpeg_activity_enabled =
        get_settings(m_dev_id).vcn_activity || get_settings(m_dev_id).jpeg_activity;
    ROCPROFSYS_AMDSMI_GET(_vcn_or_jpeg_activity_enabled, amdsmi_get_gpu_metrics_info,
                          sample_handle, &_gpu_metrics);

    // move to function - end

    // Process metrics if either VCN or JPEG activity is enabled
    if(_vcn_or_jpeg_activity_enabled)
    {
        // Helper lambda to fill busy metrics from a source array
        auto fill_busy_metrics = [](auto& dest, const auto& src) {
            for(const auto& val : src)
            {
                if(val != UINT16_MAX) dest.push_back(val);
            }
        };

        if(gpu::is_vcn_activity_supported(m_dev_id) &&
           gpu::is_jpeg_activity_supported(m_dev_id))
        {
            // Both VCN and JPEG are supported - create one entry with both metrics
            xcp_metrics_t metrics;
            fill_busy_metrics(metrics.vcn_busy, _gpu_metrics.vcn_activity);
            fill_busy_metrics(metrics.jpeg_busy, _gpu_metrics.jpeg_activity);
            if(!metrics.vcn_busy.empty() || !metrics.jpeg_busy.empty())
                m_xcp_metrics.push_back(metrics);
        }
        else if(gpu::is_vcn_activity_supported(m_dev_id))
        {
            // Only VCN is supported
            xcp_metrics_t metrics;
            fill_busy_metrics(metrics.vcn_busy, _gpu_metrics.vcn_activity);
            if(!metrics.vcn_busy.empty()) m_xcp_metrics.push_back(metrics);
        }
        else if(gpu::is_jpeg_activity_supported(m_dev_id))
        {
            // Only JPEG is supported
            xcp_metrics_t metrics;
            fill_busy_metrics(metrics.jpeg_busy, _gpu_metrics.jpeg_activity);
            if(!metrics.jpeg_busy.empty()) m_xcp_metrics.push_back(metrics);
        }
        else
        {
            // Neither is supported - use XCP stats
            // Each XCP gets one entry with both its VCN and JPEG metrics
            for(const auto& xcp : _gpu_metrics.xcp_stats)
            {
                xcp_metrics_t metrics;
                fill_busy_metrics(metrics.vcn_busy, xcp.vcn_busy);
                fill_busy_metrics(metrics.jpeg_busy, xcp.jpeg_busy);
                if(!metrics.vcn_busy.empty() || !metrics.jpeg_busy.empty())
                    m_xcp_metrics.push_back(metrics);
            }
        }
    }

#undef ROCPROFSYS_AMDSMI_GET
}

void
data::print(std::ostream& _os) const
{
    std::stringstream _ss{};

#if ROCPROFSYS_USE_ROCM > 0
    _ss << "device: " << m_dev_id << ", gpu busy: = " << m_busy_perc.gfx_activity
        << "%, mm busy: = " << m_busy_perc.mm_activity
        << "%, umc busy: = " << m_busy_perc.umc_activity << "%, temp = " << m_temp
        << ", current power = " << m_power.current_socket_power
        << ", memory usage = " << m_mem_usage;
#endif
    _os << _ss.str();
}

namespace
{
std::vector<unique_ptr_t<bundle_t>*> _bundle_data{};
}

void
config()
{
    _bundle_data.resize(data::device_count, nullptr);
    for(size_t i = 0; i < data::device_count; ++i)
    {
        if(data::device_list.count(i) > 0)
        {
            _bundle_data.at(i) = &sampler_instances::get()->at(i);
            if(!*_bundle_data.at(i))
                *_bundle_data.at(i) = unique_ptr_t<bundle_t>{ new bundle_t{} };
        }
    }

    // Checking if supported
    data::get_initial().resize(data::device_count);
    for(auto itr : data::device_list)
        data::get_initial().at(itr).sample(itr);

    metadata_initialize_category();
    metadata_initialize_smi_tracks();

    for(const auto& _dev_id : data::device_list)
    {
        metadata_initialize_smi_pmc(_dev_id);
    }
}

void
sample()
{
    auto_lock_t _lk{ type_mutex<category::amd_smi>() };

    for(auto itr : data::device_list)
    {
        if(amd_smi::get_state() != State::Active) continue;
        ROCPROFSYS_DEBUG_F("Polling amd-smi for device %u...\n", itr);
        auto& _data = *_bundle_data.at(itr);
        if(!_data) continue;
        _data->emplace_back(data{ itr });
        ROCPROFSYS_DEBUG_F("    %s\n", TIMEMORY_JOIN("", _data->back()).c_str());
    }
}

void
set_state(State _v)
{
    amd_smi::get_state().store(_v);
}

std::vector<data>&
data::get_initial()
{
    static std::vector<data> _v{};
    return _v;
}

bool
data::setup()
{
    perfetto_counter_track<data>::init();
    amd_smi::set_state(State::PreInit);
    return true;
}

bool
data::shutdown()
{
    amd_smi::set_state(State::Finalized);
    return true;
}

#define GPU_METRIC(COMPONENT, ...)                                                       \
    if constexpr(tim::trait::is_available<COMPONENT>::value)                             \
    {                                                                                    \
        auto* _val = _v.get<COMPONENT>();                                                \
        if(_val)                                                                         \
        {                                                                                \
            _val->set_value(itr.__VA_ARGS__);                                            \
            _val->set_accum(itr.__VA_ARGS__);                                            \
        }                                                                                \
    }

void
data::post_process(uint32_t _dev_id)
{
    using component::sampling_gpu_busy_gfx;
    using component::sampling_gpu_busy_mm;
    using component::sampling_gpu_busy_umc;
    using component::sampling_gpu_jpeg;
    using component::sampling_gpu_memory;
    using component::sampling_gpu_power;
    using component::sampling_gpu_temp;
    using component::sampling_gpu_vcn;

    if(device_count < _dev_id) return;

    auto&       _amd_smi_v   = sampler_instances::get()->at(_dev_id);
    auto        _amd_smi     = (_amd_smi_v) ? *_amd_smi_v : std::deque<amd_smi::data>{};
    const auto& _thread_info = thread_info::get(0, InternalTID);

    ROCPROFSYS_VERBOSE(1, "Post-processing %zu amd-smi samples from device %u\n",
                       _amd_smi.size(), _dev_id);

    ROCPROFSYS_CI_THROW(!_thread_info, "Missing thread info for thread 0");
    if(!_thread_info) return;

    auto _settings = get_settings(_dev_id);

    auto use_perfetto = get_use_perfetto();
    auto use_rocpd    = get_use_rocpd();

    for(auto& itr : _amd_smi)
    {
        using counter_track = perfetto_counter_track<data>;
        if(itr.m_dev_id != _dev_id) continue;

        uint64_t _ts = itr.m_ts;
        if(!_thread_info->is_valid_time(_ts)) continue;

        double _gfxbusy = itr.m_busy_perc.gfx_activity;
        double _umcbusy = itr.m_busy_perc.umc_activity;
        double _mmbusy  = itr.m_busy_perc.mm_activity;
        double _temp    = itr.m_temp;
        double _power   = itr.m_power.current_socket_power;
        double _usage   = itr.m_mem_usage / static_cast<double>(units::megabyte);

        auto setup_perfetto_counter_tracks = [&]() {
            if(counter_track::exists(_dev_id)) return;

            auto addendum = [&](const char* _v) {
                return JOIN(" ", "GPU", _v, JOIN("", '[', _dev_id, ']'), "(S)");
            };

            auto addendum_blk = [&](std::size_t _i, const char* _metric,
                                    std::size_t xcp_idx = SIZE_MAX) {
                if(xcp_idx != SIZE_MAX)
                {
                    return JOIN(
                        " ", "GPU", JOIN("", '[', _dev_id, ']'), _metric,
                        JOIN("", "XCP_", xcp_idx, ": [", (_i < 10 ? "0" : ""), _i, ']'),
                        "(S)");
                }
                else
                {
                    return JOIN(" ", "GPU", JOIN("", '[', _dev_id, ']'), _metric,
                                JOIN("", "[", (_i < 10 ? "0" : ""), _i, ']'), "(S)");
                }
            };

            if(_settings.busy)
            {
                counter_track::emplace(_dev_id, addendum("GFX Busy"), "%");
                counter_track::emplace(_dev_id, addendum("UMC Busy"), "%");
                counter_track::emplace(_dev_id, addendum("MM Busy"), "%");
            }
            if(_settings.temp)
            {
                counter_track::emplace(_dev_id, addendum("Temperature"), "deg C");
            }
            if(_settings.power)
            {
                counter_track::emplace(_dev_id, addendum("Current Power"), "watts");
            }
            if(_settings.mem_usage)
            {
                counter_track::emplace(_dev_id, addendum("Memory Usage"), "megabytes");
            }
            if(_settings.vcn_activity)
            {
                if(itr.m_xcp_metrics.empty())
                {
                    ROCPROFSYS_VERBOSE(
                        1, "No VCN activity data collected from device %u\n", _dev_id);
                }
                else if(gpu::is_vcn_activity_supported(_dev_id))
                {
                    // For VCN activity, use simple indexing
                    for(std::size_t i = 0; i < std::size(itr.m_xcp_metrics[0].vcn_busy);
                        ++i)
                        counter_track::emplace(_dev_id, addendum_blk(i, "VCN Activity"),
                                               "%");
                }
                else
                {
                    for(std::size_t xcp = 0; xcp < std::size(itr.m_xcp_metrics); ++xcp)
                    {
                        for(std::size_t i = 0;
                            i < std::size(itr.m_xcp_metrics[xcp].vcn_busy); ++i)
                        {
                            counter_track::emplace(
                                _dev_id, addendum_blk(i, "VCN Activity", xcp), "%");
                        }
                    }
                }
            }
            if(_settings.jpeg_activity)
            {
                if(itr.m_xcp_metrics.empty())
                {
                    ROCPROFSYS_VERBOSE(
                        1, "No JPEG activity data collected from device %u\n", _dev_id);
                }
                else if(gpu::is_jpeg_activity_supported(_dev_id))
                {
                    for(std::size_t i = 0; i < std::size(itr.m_xcp_metrics[0].jpeg_busy);
                        ++i)
                        counter_track::emplace(_dev_id, addendum_blk(i, "JPEG Activity"),
                                               "%");
                }
                else
                {
                    for(std::size_t xcp = 0; xcp < std::size(itr.m_xcp_metrics); ++xcp)
                    {
                        for(std::size_t i = 0;
                            i < std::size(itr.m_xcp_metrics[xcp].jpeg_busy); ++i)
                            counter_track::emplace(
                                _dev_id, addendum_blk(i, "JPEG Activity", xcp), "%");
                    }
                }
            }
        };

        auto write_perfetto_metrics = [&]() {
            size_t track_index = 0;

            if(_settings.busy)
            {
                TRACE_COUNTER("device_busy_gfx",
                              counter_track::at(_dev_id, track_index++), _ts, _gfxbusy);
                TRACE_COUNTER("device_busy_umc",
                              counter_track::at(_dev_id, track_index++), _ts, _umcbusy);
                TRACE_COUNTER("device_busy_mm", counter_track::at(_dev_id, track_index++),
                              _ts, _mmbusy);
            }
            if(_settings.temp)
            {
                TRACE_COUNTER("device_temp", counter_track::at(_dev_id, track_index++),
                              _ts, _temp);
            }
            if(_settings.power)
            {
                TRACE_COUNTER("device_power", counter_track::at(_dev_id, track_index++),
                              _ts, _power);
            }
            if(_settings.mem_usage)
            {
                TRACE_COUNTER("device_memory_usage",
                              counter_track::at(_dev_id, track_index++), _ts, _usage);
            }

            if(_settings.vcn_activity && !itr.m_xcp_metrics.empty())
            {
                // Iterate over all XCPs and their VCN busy/activity values
                for(const auto& metrics : itr.m_xcp_metrics)
                {
                    for(const auto& vcn_val : metrics.vcn_busy)
                    {
                        TRACE_COUNTER("device_vcn_activity",
                                      counter_track::at(_dev_id, track_index++), _ts,
                                      vcn_val);
                    }
                }
            }

            if(_settings.jpeg_activity && !itr.m_xcp_metrics.empty())
            {
                // Iterate over all XCPs and their JPEG busy/activity values
                for(const auto& metrics : itr.m_xcp_metrics)
                {
                    for(const auto& jpeg_val : metrics.jpeg_busy)
                    {
                        TRACE_COUNTER("device_jpeg_activity",
                                      counter_track::at(_dev_id, track_index++), _ts,
                                      jpeg_val);
                    }
                }
            }
        };

        if(use_perfetto)
        {
            setup_perfetto_counter_tracks();
            write_perfetto_metrics();
        }

        if(use_rocpd)
        {
            rocpd_process_smi_pmc_events(_dev_id, _settings, _ts, _mmbusy, _temp, _power,
                                         _usage);
        }
    }
}

//--------------------------------------------------------------------------------------//

void
setup()
{
    auto_lock_t _lk{ type_mutex<category::amd_smi>() };

    if(is_initialized() || !get_use_amd_smi()) return;

    ROCPROFSYS_SCOPED_SAMPLING_ON_CHILD_THREADS(false);

    if(!gpu::initialize_amdsmi())
    {
        ROCPROFSYS_WARNING_F(0,
                             "AMD SMI is not available. Disabling AMD SMI sampling...");
        return;
    }

    amdsmi_version_t _version = get_version();
    ROCPROFSYS_VERBOSE_F(0, "AMD SMI version: %u.%u.%u - str: %s.\n", _version.major,
                         _version.minor, _version.release, _version.build);

    // func get sampling devices
    data::device_count = gpu::device_count();

    auto _devices_v = get_sampling_gpus();
    for(auto& itr : _devices_v)
        itr = tolower(itr);
    if(_devices_v == "off")
        _devices_v = "none";
    else if(_devices_v == "on")
        _devices_v = "all";
    bool _all_devices = _devices_v.find("all") != std::string::npos || _devices_v.empty();
    bool _no_devices  = _devices_v.find("none") != std::string::npos;

    std::set<uint32_t> _devices = {};
    auto               _emplace = [&_devices](auto idx) {
        if(idx < data::device_count) _devices.emplace(idx);
    };

    if(_all_devices)
    {
        for(uint32_t i = 0; i < data::device_count; ++i)
            _emplace(i);
    }
    else if(!_no_devices)
    {
        auto _enabled = tim::delimit(_devices_v, ",; \t");
        for(auto&& itr : _enabled)
        {
            if(itr.find_first_not_of("0123456789-") != std::string::npos)
            {
                ROCPROFSYS_THROW("Invalid GPU specification: '%s'. Only numerical values "
                                 "(e.g., 0) or ranges (e.g., 0-7) are permitted.",
                                 itr.c_str());
            }

            if(itr.find('-') != std::string::npos)
            {
                auto _v = tim::delimit(itr, "-");
                ROCPROFSYS_CONDITIONAL_THROW(_v.size() != 2,
                                             "Invalid GPU range specification: '%s'. "
                                             "Required format N-M, e.g. 0-4",
                                             itr.c_str());
                for(auto i = std::stoul(_v.at(0)); i < std::stoul(_v.at(1)); ++i)
                    _emplace(i);
            }
            else
            {
                _emplace(std::stoul(itr));
            }
        }
    }

    data::device_list = _devices;

    // get enabled metrics sampling
    auto _metrics = get_setting_value<std::string>("ROCPROFSYS_AMD_SMI_METRICS");

    try
    {
        for(auto itr : _devices)
        {
            // Enable selected metrics only
            if((_metrics && !_metrics->empty()) && (*_metrics != "all"))
            {
                using key_pair_t     = std::pair<std::string_view, bool&>;
                const auto supported = std::unordered_map<std::string_view, bool&>{
                    key_pair_t{ "busy", get_settings(itr).busy },
                    key_pair_t{ "temp", get_settings(itr).temp },
                    key_pair_t{ "power", get_settings(itr).power },
                    key_pair_t{ "mem_usage", get_settings(itr).mem_usage },
                    key_pair_t{ "vcn_activity", get_settings(itr).vcn_activity },
                    key_pair_t{ "jpeg_activity", get_settings(itr).jpeg_activity },
                };

                // Initialize all metrics to false
                for(auto& it : supported)
                    it.second = false;

                // Parse list of metrics enabled by the user
                if(*_metrics != "none")
                {
                    for(const auto& metric : tim::delimit(*_metrics, ",;:\t\n "))
                    {
                        auto iitr = supported.find(metric);
                        if(iitr == supported.end())
                            ROCPROFSYS_FAIL_F("unsupported amd-smi metric: %s\n",
                                              metric.c_str());
                        ROCPROFSYS_VERBOSE_F(
                            1, "Enabling amd-smi metric '%s' on device [%u]\n",
                            metric.c_str(), itr);
                        iitr->second = true;
                    }
                }
            }
        }

        is_initialized() = true;
        data::setup();

    } catch(std::runtime_error& _e)
    {
        ROCPROFSYS_VERBOSE(0, "Exception thrown when initializing amd-smi: %s\n",
                           _e.what());
        data::device_list = {};
    }
}

void
shutdown()
{
    auto_lock_t _lk{ type_mutex<category::amd_smi>() };

    if(!is_initialized()) return;
    ROCPROFSYS_VERBOSE_F(1, "Shutting down amd-smi...\n");

    try
    {
        if(data::shutdown())
        {
            ROCPROFSYS_AMD_SMI_CALL(amdsmi_shut_down());
        }
    } catch(std::runtime_error& _e)
    {
        ROCPROFSYS_VERBOSE(0, "Exception thrown when shutting down amd-smi: %s\n",
                           _e.what());
    }

    is_initialized() = false;
}

void
post_process()
{
    for(auto itr : data::device_list)
    {
        ROCPROFSYS_VERBOSE(2, "Post-processing amd-smi data for device: %d", itr);
        data::post_process(itr);
    }
}

uint32_t
device_count()
{
    return gpu::device_count();
}
}  // namespace amd_smi
}  // namespace rocprofsys

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_busy_gfx>),
    true, double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_busy_umc>),
    true, double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_busy_mm>),
    true, double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_temp>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_power>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_memory>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_vcn>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_jpeg>), true,
    double)
