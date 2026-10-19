#include <mcp/types.hpp>
#include <mcp/version.hpp>

namespace mcp {

Json member(const Json& obj, std::string_view key) {
  const Json* p = obj.directMemberPtr(std::string_view(key));
  return p ? *p : Json(nullptr);
}

std::optional<RequestId> decodeRequestId(const Json& v) {
  if (v.isString()) {
    return RequestId(std::string(v.toString()));
  }
  if (v.isNumber()) {
    // Integer literal (incl. full int64 range): exact, no floating point.
    if (v.isIntegral()) return RequestId(v.toInt64());
    // Double-lexeme number: still accepted when it is mathematically integral
    // (2.0 / 42e0), but range-check BEFORE converting - static_cast<int64_t> on
    // an out-of-range double is UB and silently yields INT64_MIN.
    const double d = v.toDouble();
    constexpr double kMin = -9223372036854775808.0;  // -2^63
    constexpr double kMaxExclusive = 9223372036854775808.0;  // 2^63
    if (!(d >= kMin && d < kMaxExclusive)) return std::nullopt;
    const int64_t i = static_cast<int64_t>(d);
    if (static_cast<double>(i) != d) return std::nullopt;
    return RequestId(i);
  }
  return std::nullopt;
}

Json encodeRequestId(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return Json(*i);
  return Json::str(std::get<std::string>(id));
}

std::string resultTypeOf(const Json& result) {
  if (!result.isObject()) return std::string(kResultTypeComplete);
  // DIRECT member lookup: resultType sits at the top level, and operator[]
  // would deep-search a nested resultType inside `content` when it is absent.
  const Json rt = member(result, kResultTypeKey);
  if (!rt.isString()) return std::string(kResultTypeComplete);
  return std::string(rt.toString());
}

// ---- MRTR (basic/patterns/mrtr.md) -------------------------------------------

bool isInputRequiredResult(const Json& result) {
  return resultTypeOf(result) == std::string(kResultTypeInputRequired);
}

std::string requestStateOf(const Json& result) {
  const Json rs = member(result, kRequestStateKey);
  return rs.isString() ? std::string(rs.toString()) : std::string();
}

Json inputRequestsOf(const Json& result) {
  const Json ir = member(result, kInputRequestsKey);
  return ir.isObject() ? ir : Json(nullptr);
}

std::string_view mrtrCapabilityFor(std::string_view method) {
  if (method == "elicitation/create") return "elicitation";
  if (method == "sampling/createMessage") return "sampling";
  if (method == "roots/list") return "roots";
  return {};
}

// ---- Implementation ----------------------------------------------------------

Json encodeImplementation(const Implementation& impl) {
  Json j;
  j.add("name", Json::str(impl.name));
  if (!impl.title.empty()) j.add("title", Json::str(impl.title));
  j.add("version", Json::str(impl.version));
  if (!impl.description.empty()) j.add("description", Json::str(impl.description));
  if (!impl.websiteUrl.empty()) j.add("websiteUrl", Json::str(impl.websiteUrl));
  if (!impl.icons.isNull()) j.add("icons", impl.icons);
  return j;
}

std::optional<Implementation> decodeImplementation(const Json& v) {
  if (!v.isObject()) return std::nullopt;
  const Json name = member(v, "name");
  const Json version = member(v, "version");
  if (!name.isString() || !version.isString()) return std::nullopt;

  Implementation impl;
  impl.name = std::string(name.toString());
  impl.version = std::string(version.toString());

  const Json title = member(v, "title");
  if (title.isString()) impl.title = std::string(title.toString());
  const Json description = member(v, "description");
  if (description.isString()) impl.description = std::string(description.toString());
  const Json websiteUrl = member(v, "websiteUrl");
  if (websiteUrl.isString()) impl.websiteUrl = std::string(websiteUrl.toString());
  const Json icons = member(v, "icons");
  if (icons.isArray()) impl.icons = icons;
  return impl;
}

// ---- per-request metadata ------------------------------------------------------

McpResult<RequestMeta> decodeRequestMeta(const Json& params) {
  if (!params.isObject()) {
    return Err(McpErrorCode::InvalidParams, "request params must be an object");
  }
  const Json meta = member(params, "_meta");
  if (!meta.isObject()) {
    return Err(McpErrorCode::InvalidParams,
               "request is missing the required _meta object");
  }
  const Json pv = member(meta, kMetaProtocolVersion);
  if (!pv.isString() || pv.toString().empty()) {
    return Err(McpErrorCode::InvalidParams,
               "request _meta is missing io.modelcontextprotocol/protocolVersion");
  }
  const Json caps = member(meta, kMetaClientCapabilities);
  if (!caps.isObject()) {
    return Err(McpErrorCode::InvalidParams,
               "request _meta is missing io.modelcontextprotocol/clientCapabilities");
  }
  RequestMeta m;
  m.protocolVersion = std::string(pv.toString());
  m.clientCapabilities = caps;
  const Json info = member(meta, kMetaClientInfo);
  if (info.isObject()) m.clientInfo = decodeImplementation(info);
  const Json token = member(meta, kMetaProgressToken);
  if (token.isString() || token.isNumber()) m.progressToken = decodeRequestId(token);
  return Ok(std::move(m));
}

// Relaxed decoding shared by decodeRequestMetaRelaxed: a present-but-partial
// _meta is filled with defaults and marked !complete instead of failing. Only
// a non-object _meta (or non-object params) is an error.
McpResult<RequestMeta> decodeRequestMetaRelaxed(const Json& params,
                                                std::string_view defaultVersion) {
  // The fill-in version must be one the SERVER actually supports: a legacy
  // server that only advertises 2025-03-26 would otherwise see its very first
  // meta-less request rejected with -32022 (Latest is not in its set).
  const std::string fillVersion =
      !defaultVersion.empty() ? std::string(defaultVersion)
                              : std::string(toString(ProtocolVersion::Latest));
  // Parameterless methods (tools/list, prompts/list, ...) send params:null
  // (normalized to Json(nullptr) by the JSON-RPC layer). Treat null params
  // as an empty object so legacy clients can call them without metadata.
  const bool noParams = params.isNull();
  if (!params.isObject() && !noParams) {
    return Err(McpErrorCode::InvalidParams, "request params must be an object");
  }
  const bool hasMeta = params.isObject() && params.contains(std::string("_meta"));
  if (!hasMeta) {
    // No _meta at all: caller (legacy mode) serves with defaults.
    RequestMeta m;
    m.protocolVersion = fillVersion;
    m.clientCapabilities = Json(R"({})");
    m.complete = false;
    return Ok(std::move(m));
  }
  const Json meta = member(params, "_meta");
  if (!meta.isObject()) {
    // _meta present but not an object: nothing sensible to decode.
    return Err(McpErrorCode::InvalidParams,
               "request _meta must be an object when present");
  }
  const Json pv = member(meta, kMetaProtocolVersion);
  const Json caps = member(meta, kMetaClientCapabilities);
  const bool haveVersion = pv.isString() && !pv.toString().empty();
  const bool haveCaps = caps.isObject();
  RequestMeta m;
  m.protocolVersion =
      haveVersion ? std::string(pv.toString()) : fillVersion;
  m.clientCapabilities = haveCaps ? caps : Json(R"({})");
  m.complete = haveVersion && haveCaps;
  const Json info = member(meta, kMetaClientInfo);
  if (info.isObject()) m.clientInfo = decodeImplementation(info);
  const Json token = member(meta, kMetaProgressToken);
  if (token.isString() || token.isNumber()) m.progressToken = decodeRequestId(token);
  return Ok(std::move(m));
}

Json encodeRequestMeta(std::string_view protocolVersion,
                       const Json& clientCapabilities,
                       const std::optional<Implementation>& clientInfo,
                       const std::optional<RequestId>& progressToken) {
  Json meta;
  meta.add(std::string(kMetaProtocolVersion), Json::str(std::string(protocolVersion)));
  meta.add(std::string(kMetaClientCapabilities), clientCapabilities);
  if (clientInfo) meta.add(std::string(kMetaClientInfo), encodeImplementation(*clientInfo));
  if (progressToken) {
    meta.add(std::string(kMetaProgressToken), encodeRequestId(*progressToken));
  }
  Json params;
  params.add("_meta", std::move(meta));
  return params;
}

// ---- capability probes ---------------------------------------------------------

bool capabilitiesInclude(const Json& capabilities, std::string_view key) {
  if (!capabilities.isObject()) return false;
  const Json m = member(capabilities, key);
  return m.isObject() || m.isTrue();
}

bool capabilitiesIncludeExtension(const Json& capabilities, std::string_view extensionId) {
  if (!capabilities.isObject()) return false;
  const Json extensions = member(capabilities, "extensions");
  return extensions.isObject() && member(extensions, extensionId).isObject();
}

}  // namespace mcp
