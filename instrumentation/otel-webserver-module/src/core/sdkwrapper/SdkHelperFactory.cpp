/*
* Copyright 2021 AppDynamics LLC. 
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "sdkwrapper/SdkHelperFactory.h"
#include "sdkwrapper/SdkConstants.h"
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/simple_processor.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/sdk/trace/samplers/always_on.h"
#include "opentelemetry/sdk/trace/samplers/always_off.h"
#include "opentelemetry/sdk/trace/samplers/parent.h"
#include "opentelemetry/sdk/trace/samplers/trace_id_ratio.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter.h"
#include "opentelemetry/baggage/propagation/baggage_propagator.h"

#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter.h"
#include "opentelemetry/exporters/ostream/metric_exporter.h"
#  include "opentelemetry/metrics/provider.h"
#  include "opentelemetry/sdk/metrics/aggregation/default_aggregation.h"
#  include "opentelemetry/sdk/metrics/aggregation/histogram_aggregation.h"
#  include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h"
#  include "opentelemetry/sdk/metrics/meter.h"
#  include "opentelemetry/sdk/metrics/meter_provider.h"

#include <module_version.h>
#include <fstream>
#include <iostream>

namespace appd {
namespace core {
namespace sdkwrapper {

// NOTE : all the validation checks related to otel inputs are done in the helper factory.
// So, these constants are not required outside of this execution unit
namespace {
  constexpr const char* OTLP_EXPORTER_TYPE = "otlp";
  constexpr const char* OSSTREAM_EXPORTER_TYPE = "osstream";
  constexpr const char* SIMPLE_PROCESSOR = "simple";
  constexpr const char* BATCH_PROCESSOR = "batch";
  constexpr const char* ALWAYS_ON_SAMPLER = "always_on";
  constexpr const char* ALWAYS_OFF_SAMPLER = "always_off";
  constexpr const char* PARENT_BASED_SAMPLER = "parent";
  constexpr const char* TRACE_ID_RATIO_BASED_SAMPLER = "trace_id_ratio";
}

class MeasurementFetcher
{
public:
  static void Fetcher(opentelemetry::metrics::ObserverResult<long> &observer_result, void *state)
  {
    long val = (rand() % 700) + 1;
    observer_result.Observe(val /*, labelkv*/);
  }
};

SdkHelperFactory::SdkHelperFactory(
    std::shared_ptr<TenantConfig> config,
    const AgentLogger& logger) :
    mLogger(logger)
{
    // Create exporter, processor and provider.

    // TODO:: Use constant expressions
    LOG4CXX_INFO(mLogger, "ServiceNamespace: " << config->getServiceNamespace() <<
        " ServiceName: " << config->getServiceName() <<
        " ServiceInstanceId: " << config->getServiceInstanceId());
    sdk::resource::ResourceAttributes attributes;
    attributes[kServiceName] = config->getServiceName();
    attributes[kServiceNamespace] = config->getServiceNamespace();
    attributes[kServiceInstanceId] = config->getServiceInstanceId();

    // NOTE : resource attribute values are nostd::variant and so we need to explicitely set it to std::string
    std::string libraryVersion = MODULE_VERSION;

    // NOTE : InstrumentationLibrary code is incomplete for the otlp exporter in sdk.
    // So, we need to pass libraryName and libraryVersion as resource attributes.
    // Ref : https://github.com/open-telemetry/opentelemetry-cpp/blob/main/exporters/otlp/src/otlp_recordable.cc
    attributes[kOtelLibraryName] = config->getOtelLibraryName();
    attributes[kOtelLibraryVersion] = libraryVersion;

    auto exporter = GetExporter(config);
    auto processor = GetSpanProcessor(config, std::move(exporter));
    auto sampler = GetSampler(config);

    mTracerProvider = opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(
      new opentelemetry::sdk::trace::TracerProvider(
            std::move(processor),
            opentelemetry::sdk::resource::Resource::Create(attributes),
            std::move(sampler)
            ));

    mTracer = mTracerProvider->GetTracer(config->getOtelLibraryName(), libraryVersion);
    LOG4CXX_INFO(mLogger,
        "Tracer created with LibraryName: " << config->getOtelLibraryName() <<
        " and LibraryVersion " << libraryVersion);

    // Adding trace propagator
    using MapHttpTraceCtx = opentelemetry::trace::propagation::HttpTraceContext;
    mPropagators.push_back(
        std::unique_ptr<MapHttpTraceCtx>(new MapHttpTraceCtx()));

    // Adding Baggage Propagator
    using BaggagePropagator = opentelemetry::baggage::propagation::BaggagePropagator;
    mPropagators.push_back(
        std::unique_ptr<BaggagePropagator>(new BaggagePropagator()));

    namespace metrics_sdk      = opentelemetry::sdk::metrics;
    namespace metrics_api      = opentelemetry::metrics;

    opentelemetry::exporter::otlp::OtlpGrpcMetricExporterOptions opts;
    opts.endpoint = "docker.for.mac.localhost:4317";
    //auto mexporter = std::unique_ptr<opentelemetry::exporter::otlp::OtlpGrpcMetricExporter>(
    //        new opentelemetry::exporter::otlp::OtlpGrpcMetricExporter());
    auto mexporter = std::unique_ptr<opentelemetry::exporter::metrics::OStreamMetricExporter>(
            new opentelemetry::exporter::metrics::OStreamMetricExporter());

    std::string version{"1.2.0"};
    std::string schema{"https://opentelemetry.io/schemas/1.2.0"};

    // Initialize and set the global MeterProvider
    metrics_sdk::PeriodicExportingMetricReaderOptions options;
    options.export_interval_millis = std::chrono::milliseconds(10000);
    options.export_timeout_millis  = std::chrono::milliseconds(500);
    std::unique_ptr<metrics_sdk::MetricReader> reader{
        new metrics_sdk::PeriodicExportingMetricReader(std::move(mexporter), options)};
    auto provider = std::shared_ptr<metrics_api::MeterProvider>(new metrics_sdk::MeterProvider());
    auto p        = std::static_pointer_cast<metrics_sdk::MeterProvider>(provider);
    p->AddMetricReader(std::move(reader));

    // counter view
    std::string name {"TotalRequests"};
    std::string counter_name = name + "_counter";
    std::unique_ptr<metrics_sdk::InstrumentSelector> instrument_selector{
        new metrics_sdk::InstrumentSelector(metrics_sdk::InstrumentType::kCounter, counter_name)};
    std::unique_ptr<metrics_sdk::MeterSelector> meter_selector{
        new metrics_sdk::MeterSelector(name, version, schema)};
    std::unique_ptr<metrics_sdk::View> sum_view{
        new metrics_sdk::View{name, "description", metrics_sdk::AggregationType::kSum}};
    p->AddView(std::move(instrument_selector), std::move(meter_selector), std::move(sum_view));

    metrics_api::Provider::SetMeterProvider(provider);

    auto nprovider                               = metrics_api::Provider::GetMeterProvider();
    nostd::shared_ptr<metrics_api::Meter> meter = nprovider->GetMeter(name, "1.2.0");
    meter->CreateLongObservableCounter(counter_name, appd::core::sdkwrapper::MeasurementFetcher::Fetcher);
}

OtelTracer SdkHelperFactory::GetTracer()
{
    return mTracer;
}

OtelPropagators& SdkHelperFactory::GetPropagators()
{
  return mPropagators;
}

OtelSpanExporter SdkHelperFactory::GetExporter(
    std::shared_ptr<TenantConfig> config)
{
    auto exporter = std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>{};
    auto type = config->getOtelExporterType();

    if (type == OSSTREAM_EXPORTER_TYPE) {
        exporter.reset(new opentelemetry::exporter::trace::OStreamSpanExporter);
    } else {
        if (type != OTLP_EXPORTER_TYPE) {
          // default is otlp exporter
          LOG4CXX_WARN(mLogger, "Received unknown exporter type: " << type << ". Will create default(otlp) exporter");
          type = OTLP_EXPORTER_TYPE;
        }
        opentelemetry::exporter::otlp::OtlpGrpcExporterOptions opts;
        opts.endpoint = config->getOtelExporterEndpoint();
        if (config->getOtelSslEnabled()) {
            opts.use_ssl_credentials = config->getOtelSslEnabled();
            opts.ssl_credentials_cacert_path = config->getOtelSslCertPath();
            LOG4CXX_TRACE(mLogger, "Ssl Credentials are enabled for exporter, path: "
                << opts.ssl_credentials_cacert_path);
        }
        exporter.reset(new opentelemetry::exporter::otlp::OtlpGrpcExporter(opts));
    }

    LOG4CXX_INFO(mLogger, "Exporter created with ExporterType: "
        << type);
    return std::move(exporter);
}

OtelSpanProcessor SdkHelperFactory::GetSpanProcessor(
    std::shared_ptr<TenantConfig> config,
    OtelSpanExporter exporter)
{
    auto processor = OtelSpanProcessor{};
    auto type = config->getOtelProcessorType();
    if (type == SIMPLE_PROCESSOR) {
        processor.reset(
                new opentelemetry::sdk::trace::SimpleSpanProcessor(std::move(exporter)));
    } else {
        if (type != BATCH_PROCESSOR) {
           // default is batch processor
           LOG4CXX_WARN(mLogger, "Received unknown processor type: " << type << ". Will create default(batch) processor");
           type = BATCH_PROCESSOR;
        }
        opentelemetry::sdk::trace::BatchSpanProcessorOptions options;
        processor.reset(
            new opentelemetry::sdk::trace::BatchSpanProcessor(std::move(exporter), options));
    }

    LOG4CXX_INFO(mLogger, "Processor created with ProcessorType: "
        << type);
    return processor;
}

OtelSampler SdkHelperFactory::GetSampler(
    std::shared_ptr<TenantConfig> config)
{
    auto sampler = OtelSampler{};
    auto type = config->getOtelSamplerType();

    if (type == ALWAYS_OFF_SAMPLER) {
        sampler.reset(new sdk::trace::AlwaysOffSampler);
    } else if (type == TRACE_ID_RATIO_BASED_SAMPLER) { // TODO
        ;
    } else if (type == PARENT_BASED_SAMPLER) { // TODO
        ;
    } else {
        if (type != ALWAYS_ON_SAMPLER) {
          // default is always_on sampler
          LOG4CXX_WARN(mLogger, "Received unknown sampler type: " << type << ". Will create default(always_on) sampler");
          type = ALWAYS_ON_SAMPLER;
        }
        sampler.reset(new sdk::trace::AlwaysOnSampler);
    }

    LOG4CXX_INFO(mLogger, "Sampler created with SamplerType : " <<
        type);
    return sampler;
}

} //sdkwrapper
} //core
} //appd

