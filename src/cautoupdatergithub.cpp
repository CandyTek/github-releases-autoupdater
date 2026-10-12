#include "cautoupdatergithub.h"
#include "updateinstaller.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "maddy/parser.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr auto kUserAgent = L"github-releases-autoupdater/1.0";

std::wstring utf8ToWide(std::string_view value)
{
	if (value.empty())
		return {};

	const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (required <= 0)
		throw std::runtime_error("Failed to convert UTF-8 string to UTF-16.");

	std::wstring result(static_cast<std::size_t>(required), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required) != required)
		throw std::runtime_error("Incomplete UTF-8 to UTF-16 conversion.");

	return result;
}

std::string wideToUtf8(std::wstring_view value)
{
	if (value.empty())
		return {};

	const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0)
		throw std::runtime_error("Failed to convert UTF-16 string to UTF-8.");

	std::string result(static_cast<std::size_t>(required), '\0');
	if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) != required)
		throw std::runtime_error("Incomplete UTF-16 to UTF-8 conversion.");

	return result;
}

std::string trimVersionPrefix(std::string version)
{
	if (version.rfind(".v", 0) == 0)
		version.erase(0, 2);
	else if (!version.empty() && version.front() == 'v')
		version.erase(0, 1);

	return version;
}

bool isDigit(char ch)
{
	return ch >= '0' && ch <= '9';
}

int compareNaturalAsciiInsensitive(std::string_view left, std::string_view right)
{
	std::size_t leftIndex = 0;
	std::size_t rightIndex = 0;

	while (leftIndex < left.size() && rightIndex < right.size())
	{
		const bool leftIsDigit = isDigit(left[leftIndex]);
		const bool rightIsDigit = isDigit(right[rightIndex]);
		if (leftIsDigit && rightIsDigit)
		{
			std::size_t leftNumberEnd = leftIndex;
			while (leftNumberEnd < left.size() && isDigit(left[leftNumberEnd]))
				++leftNumberEnd;

			std::size_t rightNumberEnd = rightIndex;
			while (rightNumberEnd < right.size() && isDigit(right[rightNumberEnd]))
				++rightNumberEnd;

			auto leftDigits = left.substr(leftIndex, leftNumberEnd - leftIndex);
			auto rightDigits = right.substr(rightIndex, rightNumberEnd - rightIndex);

			const auto leftNonZero = leftDigits.find_first_not_of('0');
			const auto rightNonZero = rightDigits.find_first_not_of('0');

			leftDigits = leftNonZero == std::string_view::npos ? std::string_view{"0"} : leftDigits.substr(leftNonZero);
			rightDigits = rightNonZero == std::string_view::npos ? std::string_view{"0"} : rightDigits.substr(rightNonZero);

			if (leftDigits.size() != rightDigits.size())
				return leftDigits.size() < rightDigits.size() ? -1 : 1;

			const int numericComparison = leftDigits.compare(rightDigits);
			if (numericComparison != 0)
				return numericComparison < 0 ? -1 : 1;

			leftIndex = leftNumberEnd;
			rightIndex = rightNumberEnd;
			continue;
		}

		const auto leftLower = static_cast<char>(std::tolower(static_cast<unsigned char>(left[leftIndex])));
		const auto rightLower = static_cast<char>(std::tolower(static_cast<unsigned char>(right[rightIndex])));
		if (leftLower != rightLower)
			return leftLower < rightLower ? -1 : 1;

		++leftIndex;
		++rightIndex;
	}

	if (leftIndex == left.size() && rightIndex == right.size())
		return 0;

	return leftIndex == left.size() ? -1 : 1;
}

bool naturalVersionLessThan(const std::string& left, const std::string& right)
{
	return compareNaturalAsciiInsensitive(left, right) < 0;
}

std::string removeCarriageReturns(std::string value)
{
	value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
	return value;
}

bool endsWithCaseInsensitive(std::string_view value, std::string_view suffix)
{
	if (suffix.size() > value.size())
		return false;

	const auto offset = value.size() - suffix.size();
	for (std::size_t index = 0; index < suffix.size(); ++index)
	{
		const auto left = static_cast<char>(std::tolower(static_cast<unsigned char>(value[offset + index])));
		const auto right = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[index])));
		if (left != right)
			return false;
	}

	return true;
}

bool hasPreferredPortableAssetExtension(std::string_view value)
{
	return endsWithCaseInsensitive(value, ".zip");
}

bool hasSupportedInstallerExtension(std::string_view value)
{
	return endsWithCaseInsensitive(value, ".exe") || endsWithCaseInsensitive(value, ".msi");
}

// GitHub reports asset digests as "sha256:<64 lowercase hex chars>". Returns the hex part, or empty if absent/unsupported.
std::string parseSha256Digest(std::string_view digest)
{
	constexpr std::string_view kPrefix = "sha256:";
	if (digest.size() != kPrefix.size() + 64 || digest.substr(0, kPrefix.size()) != kPrefix)
		return {};

	std::string hex(digest.substr(kPrefix.size()));
	for (char& ch : hex)
	{
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		const bool isHex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		if (!isHex)
			return {};
	}

	return hex;
}

std::uint64_t parseUnsigned(std::string_view value)
{
	std::uint64_t result = 0;
	const auto status = std::from_chars(value.data(), value.data() + value.size(), result);
	return status.ec == std::errc{} && status.ptr == value.data() + value.size() ? result : 0;
}

struct BCryptAlgorithmHandle {
	BCRYPT_ALG_HANDLE handle = nullptr;
	~BCryptAlgorithmHandle()
	{
		if (handle)
			BCryptCloseAlgorithmProvider(handle, 0);
	}
};

struct BCryptHashHandle {
	BCRYPT_HASH_HANDLE handle = nullptr;
	~BCryptHashHandle()
	{
		if (handle)
			BCryptDestroyHash(handle);
	}
};

std::string computeSha256Hex(std::string_view data)
{
	BCryptAlgorithmHandle algorithm;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
		throw std::runtime_error("Failed to open the SHA-256 provider.");

	BCryptHashHandle hash;
	if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0)))
		throw std::runtime_error("Failed to create the SHA-256 hash object.");

	std::size_t offset = 0;
	while (offset < data.size())
	{
		const std::size_t chunk = std::min<std::size_t>(data.size() - offset, 1u << 20);
		auto* bytes = reinterpret_cast<PUCHAR>(const_cast<char*>(data.data() + offset));
		if (!BCRYPT_SUCCESS(BCryptHashData(hash.handle, bytes, static_cast<ULONG>(chunk), 0)))
			throw std::runtime_error("Failed to hash the downloaded update data.");
		offset += chunk;
	}

	std::array<UCHAR, 32> digest{};
	if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0)))
		throw std::runtime_error("Failed to finish the SHA-256 hash.");

	static constexpr char kHexDigits[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(digest.size() * 2);
	for (const UCHAR byte : digest)
	{
		hex.push_back(kHexDigits[byte >> 4]);
		hex.push_back(kHexDigits[byte & 0x0F]);
	}

	return hex;
}

std::wstring getDownloadFileExtension(std::string_view updateUrl)
{
	const auto queryPos = updateUrl.find('?');
	const std::string_view normalized = queryPos == std::string_view::npos ? updateUrl : updateUrl.substr(0, queryPos);
	const std::filesystem::path path(utf8ToWide(std::string(normalized)));
	const std::wstring extension = path.extension().wstring();
	return extension.empty() ? L".bin" : extension;
}

std::string formatIsoDate(std::string_view value)
{
	if (value.size() < 10)
		return std::string(value);

	int year = 0;
	int month = 0;
	int day = 0;

	const auto yearResult = std::from_chars(value.data(), value.data() + 4, year);
	const auto monthResult = std::from_chars(value.data() + 5, value.data() + 7, month);
	const auto dayResult = std::from_chars(value.data() + 8, value.data() + 10, day);
	if (yearResult.ec != std::errc{} || monthResult.ec != std::errc{} || dayResult.ec != std::errc{})
		return std::string(value);

	static constexpr std::array<const char*, 12> kMonths{
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	if (month < 1 || month > 12 || day < 1 || day > 31)
		return std::string(value);

	std::ostringstream output;
	if (day < 10)
		output << '0';

	output << day << ' ' << kMonths[static_cast<std::size_t>(month - 1)] << ' ' << year;
	return output.str();
}

struct WinHttpHandle {
	HINTERNET handle = nullptr;

	WinHttpHandle() = default;
	explicit WinHttpHandle(HINTERNET value) : handle(value) {}

	~WinHttpHandle()
	{
		if (handle)
			WinHttpCloseHandle(handle);
	}

	WinHttpHandle(const WinHttpHandle&) = delete;
	WinHttpHandle& operator=(const WinHttpHandle&) = delete;

	WinHttpHandle(WinHttpHandle&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}

	WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
	{
		if (this != &other)
		{
			if (handle)
				WinHttpCloseHandle(handle);

			handle = std::exchange(other.handle, nullptr);
		}

		return *this;
	}

	operator HINTERNET() const
	{
		return handle;
	}
};

std::string getLastErrorMessage(const char* context)
{
	const DWORD error = GetLastError();
	LPWSTR messageBuffer = nullptr;
	const DWORD size = FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		error,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPWSTR>(&messageBuffer),
		0,
		nullptr);

	std::string message = context;
	message += ": ";
	if (size == 0 || !messageBuffer)
	{
		message += "Win32 error ";
		message += std::to_string(error);
		return message;
	}

	std::wstring_view wideMessage(messageBuffer, size);
	message += wideToUtf8(wideMessage);
	LocalFree(messageBuffer);

	while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
		message.pop_back();

	return message;
}

struct HttpResponse {
	std::string body;
	std::uint64_t contentLength = 0;
};

HttpResponse httpGet(std::string_view url, const std::function<void(std::uint64_t, std::uint64_t)>& onChunkDownloaded = {})
{
	const std::wstring wideUrl = utf8ToWide(url);

	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);

	if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components))
		throw std::runtime_error(getLastErrorMessage("Failed to parse URL"));

	std::wstring host(components.lpszHostName, components.dwHostNameLength);
	std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
	if (components.dwExtraInfoLength > 0)
		path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

	const bool isHttps = components.nScheme == INTERNET_SCHEME_HTTPS;
	const DWORD openFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;

	WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session)
		throw std::runtime_error(getLastErrorMessage("Failed to open WinHTTP session"));

	const DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, const_cast<DWORD*>(&redirectPolicy), sizeof(redirectPolicy));

	WinHttpHandle connection(WinHttpConnect(session, host.c_str(), components.nPort, 0));
	if (!connection)
		throw std::runtime_error(getLastErrorMessage("Failed to connect to remote host"));

	WinHttpHandle request(WinHttpOpenRequest(
		connection,
		L"GET",
		path.c_str(),
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		openFlags));
	if (!request)
		throw std::runtime_error(getLastErrorMessage("Failed to create HTTP request"));

	const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
	if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
		throw std::runtime_error(getLastErrorMessage("Failed to send HTTP request"));

	if (!WinHttpReceiveResponse(request, nullptr))
		throw std::runtime_error(getLastErrorMessage("Failed to receive HTTP response"));

	DWORD statusCode = 0;
	DWORD statusCodeSize = sizeof(statusCode);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
		throw std::runtime_error(getLastErrorMessage("Failed to query HTTP status code"));

	if (statusCode < 200 || statusCode >= 300)
		throw std::runtime_error("Unexpected HTTP status code: " + std::to_string(statusCode));

	std::uint64_t contentLength = 0;
	wchar_t contentLengthBuffer[64]{};
	DWORD contentLengthBufferSize = sizeof(contentLengthBuffer);
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, contentLengthBuffer, &contentLengthBufferSize, WINHTTP_NO_HEADER_INDEX))
		contentLength = std::wcstoull(contentLengthBuffer, nullptr, 10);

	HttpResponse response;
	response.contentLength = contentLength;

	std::uint64_t downloaded = 0;
	for (;;)
	{
		DWORD availableBytes = 0;
		if (!WinHttpQueryDataAvailable(request, &availableBytes))
			throw std::runtime_error(getLastErrorMessage("Failed to query available response data"));

		if (availableBytes == 0)
			break;

		std::string buffer(static_cast<std::size_t>(availableBytes), '\0');
		DWORD bytesRead = 0;
		if (!WinHttpReadData(request, buffer.data(), availableBytes, &bytesRead))
			throw std::runtime_error(getLastErrorMessage("Failed to read HTTP response data"));

		buffer.resize(bytesRead);
		response.body.append(buffer);
		downloaded += bytesRead;

		if (onChunkDownloaded)
			onChunkDownloaded(downloaded, contentLength);
	}

	return response;
}

class JsonValue {
public:
	enum class Type {
		Null,
		Boolean,
		String,
		Array,
		Object
	};

	using Array = std::vector<JsonValue>;
	using Object = std::unordered_map<std::string, JsonValue>;

	JsonValue() = default;

	static JsonValue boolean(bool value)
	{
		JsonValue result;
		result._type = Type::Boolean;
		result._boolean = value;
		return result;
	}

	static JsonValue string(std::string value)
	{
		JsonValue result;
		result._type = Type::String;
		result._string = std::move(value);
		return result;
	}

	static JsonValue array(Array value)
	{
		JsonValue result;
		result._type = Type::Array;
		result._array = std::move(value);
		return result;
	}

	static JsonValue object(Object value)
	{
		JsonValue result;
		result._type = Type::Object;
		result._object = std::move(value);
		return result;
	}

	Type type() const
	{
		return _type;
	}

	bool asBool(bool defaultValue = false) const
	{
		return _type == Type::Boolean ? _boolean : defaultValue;
	}

	const std::string& asString() const
	{
		static const std::string emptyString;
		return _type == Type::String ? _string : emptyString;
	}

	const Array& asArray() const
	{
		static const Array emptyArray;
		return _type == Type::Array ? _array : emptyArray;
	}

	const JsonValue& find(std::string_view key) const
	{
		static const JsonValue nullValue;
		if (_type != Type::Object)
			return nullValue;

		const auto iterator = _object.find(std::string(key));
		return iterator == _object.end() ? nullValue : iterator->second;
	}

private:
	Type _type = Type::Null;
	bool _boolean = false;
	std::string _string;
	Array _array;
	Object _object;
};

class JsonParser {
public:
	explicit JsonParser(std::string_view input) : _input(input) {}

	JsonValue parse()
	{
		skipWhitespace();
		JsonValue value = parseValue();
		skipWhitespace();
		if (_position != _input.size())
			throw std::runtime_error("Unexpected trailing JSON content.");

		return value;
	}

private:
	JsonValue parseValue()
	{
		if (_position >= _input.size())
			throw std::runtime_error("Unexpected end of JSON input.");

		switch (_input[_position])
		{
		case 'n':
			parseLiteral("null");
			return JsonValue{};
		case 't':
			parseLiteral("true");
			return JsonValue::boolean(true);
		case 'f':
			parseLiteral("false");
			return JsonValue::boolean(false);
		case '"':
			return JsonValue::string(parseString());
		case '[':
			return JsonValue::array(parseArray());
		case '{':
			return JsonValue::object(parseObject());
		default:
			return parsePrimitive();
		}
	}

	JsonValue parsePrimitive()
	{
		const std::size_t start = _position;
		if (_input[_position] == '-')
			++_position;

		while (_position < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_position])))
			++_position;

		if (_position < _input.size() && _input[_position] == '.')
		{
			++_position;
			while (_position < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_position])))
				++_position;
		}

		if (_position < _input.size() && (_input[_position] == 'e' || _input[_position] == 'E'))
		{
			++_position;
			if (_position < _input.size() && (_input[_position] == '+' || _input[_position] == '-'))
				++_position;

			while (_position < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_position])))
				++_position;
		}

		if (start == _position)
			throw std::runtime_error("Unsupported JSON token.");

		return JsonValue::string(std::string(_input.substr(start, _position - start)));
	}

	JsonValue::Array parseArray()
	{
		expect('[');
		skipWhitespace();

		JsonValue::Array result;
		if (peek(']'))
		{
			expect(']');
			return result;
		}

		for (;;)
		{
			skipWhitespace();
			result.push_back(parseValue());
			skipWhitespace();

			if (peek(']'))
			{
				expect(']');
				break;
			}

			expect(',');
		}

		return result;
	}

	JsonValue::Object parseObject()
	{
		expect('{');
		skipWhitespace();

		JsonValue::Object result;
		if (peek('}'))
		{
			expect('}');
			return result;
		}

		for (;;)
		{
			skipWhitespace();
			std::string key = parseString();
			skipWhitespace();
			expect(':');
			skipWhitespace();
			result.emplace(std::move(key), parseValue());
			skipWhitespace();

			if (peek('}'))
			{
				expect('}');
				break;
			}

			expect(',');
		}

		return result;
	}

	std::string parseString()
	{
		expect('"');
		std::string result;

		while (_position < _input.size())
		{
			const char ch = _input[_position++];
			if (ch == '"')
				return result;

			if (ch != '\\')
			{
				result.push_back(ch);
				continue;
			}

			if (_position >= _input.size())
				throw std::runtime_error("Incomplete JSON escape sequence.");

			const char escaped = _input[_position++];
			switch (escaped)
			{
			case '"': result.push_back('"'); break;
			case '\\': result.push_back('\\'); break;
			case '/': result.push_back('/'); break;
			case 'b': result.push_back('\b'); break;
			case 'f': result.push_back('\f'); break;
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			case 'u':
				result += parseUnicodeEscape();
				break;
			default:
				throw std::runtime_error("Unsupported JSON escape sequence.");
			}
		}

		throw std::runtime_error("Unterminated JSON string.");
	}

	std::string parseUnicodeEscape()
	{
		if (_position + 4 > _input.size())
			throw std::runtime_error("Incomplete JSON unicode escape.");

		unsigned int codePoint = 0;
		for (int index = 0; index < 4; ++index)
		{
			const char ch = _input[_position++];
			codePoint <<= 4;
			if (ch >= '0' && ch <= '9')
				codePoint |= static_cast<unsigned int>(ch - '0');
			else if (ch >= 'a' && ch <= 'f')
				codePoint |= static_cast<unsigned int>(ch - 'a' + 10);
			else if (ch >= 'A' && ch <= 'F')
				codePoint |= static_cast<unsigned int>(ch - 'A' + 10);
			else
				throw std::runtime_error("Invalid JSON unicode escape.");
		}

		if (codePoint <= 0x7F)
			return std::string(1, static_cast<char>(codePoint));
		if (codePoint <= 0x7FF)
		{
			return {
				static_cast<char>(0xC0 | (codePoint >> 6)),
				static_cast<char>(0x80 | (codePoint & 0x3F))
			};
		}

		return {
			static_cast<char>(0xE0 | (codePoint >> 12)),
			static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)),
			static_cast<char>(0x80 | (codePoint & 0x3F))
		};
	}

	void parseLiteral(std::string_view literal)
	{
		if (_input.substr(_position, literal.size()) != literal)
			throw std::runtime_error("Unexpected JSON literal.");

		_position += literal.size();
	}

	void skipWhitespace()
	{
		while (_position < _input.size() && std::isspace(static_cast<unsigned char>(_input[_position])))
			++_position;
	}

	bool peek(char expected) const
	{
		return _position < _input.size() && _input[_position] == expected;
	}

	void expect(char expected)
	{
		if (_position >= _input.size() || _input[_position] != expected)
			throw std::runtime_error("Unexpected JSON structure.");

		++_position;
	}

	std::string_view _input;
	std::size_t _position = 0;
};

CAutoUpdaterGithub::ChangeLog parseReleasesJson(
	std::string_view json,
	const std::string& currentVersion,
	const std::function<bool(const std::string&, const std::string&)>& lessThanVersion)
{
	const JsonValue root = JsonParser(json).parse();
	if (root.type() != JsonValue::Type::Array)
		throw std::runtime_error("GitHub API returned an unexpected JSON payload.");

	CAutoUpdaterGithub::ChangeLog changelog;

	for (const JsonValue& releaseValue : root.asArray())
	{
		if (releaseValue.find("draft").asBool())
			continue;

		std::string updateVersion = trimVersionPrefix(releaseValue.find("tag_name").asString());
		if (!lessThanVersion(currentVersion, updateVersion))
			continue;

		const JsonValue* chosenAsset = nullptr;
		for (const JsonValue& assetValue : releaseValue.find("assets").asArray())
		{
			const std::string& assetUrl = assetValue.find("browser_download_url").asString();
			if (hasPreferredPortableAssetExtension(assetUrl))
			{
				chosenAsset = &assetValue;
				break;
			}

			if (!chosenAsset && hasSupportedInstallerExtension(assetUrl))
				chosenAsset = &assetValue;
		}

		const std::string updateChanges = removeCarriageReturns(releaseValue.find("body").asString());
		maddy::Parser markdownParser;
		std::istringstream stream(updateChanges);
		std::string htmlChanges = markdownParser.Parse(stream);

		CAutoUpdaterGithub::VersionEntry entry;
		entry.versionString = updateVersion;
		entry.versionChanges = htmlChanges;
		entry.date = formatIsoDate(releaseValue.find("created_at").asString());
		entry.isPrerelease = releaseValue.find("prerelease").asBool();
		if (chosenAsset)
		{
			entry.versionUpdateUrl = chosenAsset->find("browser_download_url").asString();
			entry.versionUpdateSha256 = parseSha256Digest(chosenAsset->find("digest").asString());
			entry.versionUpdateSize = parseUnsigned(chosenAsset->find("size").asString());
		}

		changelog.push_back(std::move(entry));
	}

	return changelog;
}

std::wstring getCurrentModuleStem()
{
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (size == 0)
		return L"autoupdater";

	while (size == buffer.size())
	{
		buffer.resize(buffer.size() * 2, L'\0');
		size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (size == 0)
			return L"autoupdater";
	}

	buffer.resize(size);
	return std::filesystem::path(buffer).stem().wstring();
}

} // namespace

CAutoUpdaterGithub::CAutoUpdaterGithub(
	std::string githubRepositoryName,
	std::string currentVersionString,
	std::function<bool(const std::string&, const std::string&)> versionStringComparatorLessThan) :
	_repoName(std::move(githubRepositoryName)),
	_currentVersionString(std::move(currentVersionString)),
	_lessThanVersionStringComparator(versionStringComparatorLessThan ? std::move(versionStringComparatorLessThan) : naturalVersionLessThan)
{
	assert(std::count(_repoName.begin(), _repoName.end(), '/') == 1);
	assert(!_currentVersionString.empty());
}

void CAutoUpdaterGithub::setUpdateStatusListener(UpdateStatusListener* listener)
{
	_listener = listener;
}

void CAutoUpdaterGithub::checkForUpdates()
{
	try
	{
		const std::string requestUrl = "https://api.github.com/repos/" + _repoName + "/releases";
		const HttpResponse response = httpGet(requestUrl);
		const ChangeLog changelog = parseReleasesJson(response.body, _currentVersionString, _lessThanVersionStringComparator);

		if (_listener)
			_listener->onUpdateAvailable(changelog);
	}
	catch (const std::exception& exception)
	{
		notifyError(exception.what());
	}
}

void CAutoUpdaterGithub::downloadAndInstallUpdate(const VersionEntry& entry)
{
	try
	{
		const std::string& updateUrl = entry.versionUpdateUrl;
		if (updateUrl.empty())
			throw std::runtime_error("The release does not contain a downloadable update package.");

		// Refuse to install anything we cannot verify: the GitHub API publishes a SHA-256 digest for every asset.
		if (entry.versionUpdateSha256.empty())
			throw std::runtime_error("The release asset has no SHA-256 digest, refusing to install an unverified package.");

		const std::filesystem::path downloadPath = buildDownloadPath(updateUrl);
		const HttpResponse response = httpGet(updateUrl, [this](std::uint64_t downloaded, std::uint64_t total) {
			if (!_listener)
				return;

			if (total == 0)
			{
				_listener->onUpdateDownloadProgress(0.0f);
				return;
			}

			const float percentage = downloaded >= total ? 100.0f : static_cast<float>(downloaded * 100.0) / static_cast<float>(total);
			_listener->onUpdateDownloadProgress(percentage);
		});

		if (response.body.empty())
			throw std::runtime_error("The downloaded update package is empty.");

		if (entry.versionUpdateSize != 0 && response.body.size() != entry.versionUpdateSize)
		{
			throw std::runtime_error(
				"Downloaded update size mismatch: expected " + std::to_string(entry.versionUpdateSize)
				+ " bytes, received " + std::to_string(response.body.size()) + " bytes.");
		}

		const std::string actualSha256 = computeSha256Hex(response.body);
		if (actualSha256 != entry.versionUpdateSha256)
		{
			throw std::runtime_error(
				"Downloaded update SHA-256 mismatch: expected " + entry.versionUpdateSha256
				+ ", got " + actualSha256 + ". The package was not installed.");
		}

		std::ofstream file(downloadPath, std::ios::binary | std::ios::trunc);
		if (!file)
			throw std::runtime_error("Failed to open the temporary update file for writing.");

		file.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
		file.close();
		if (!file)
			throw std::runtime_error("Failed to write the downloaded update file.");

		if (_listener)
			_listener->onUpdateDownloadProgress(100.0f);

		if (!UpdateInstaller::install(downloadPath))
			throw std::runtime_error("Failed to launch the downloaded update.");

		if (_listener)
			_listener->onUpdateDownloadFinished();
	}
	catch (const std::exception& exception)
	{
		notifyError(exception.what());
	}
}

void CAutoUpdaterGithub::notifyError(const std::string& errorMessage) const
{
	if (_listener)
		_listener->onUpdateError(errorMessage);
}

std::filesystem::path CAutoUpdaterGithub::buildDownloadPath(const std::string& updateUrl) const
{
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD size = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
	if (size == 0)
		throw std::runtime_error(getLastErrorMessage("Failed to determine the temporary directory"));

	if (size > buffer.size())
	{
		buffer.resize(size, L'\0');
		size = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
		if (size == 0)
			throw std::runtime_error(getLastErrorMessage("Failed to determine the temporary directory"));
	}

	buffer.resize(size);
	std::filesystem::path tempDirectory(buffer);
	return tempDirectory / (getCurrentModuleStem() + getDownloadFileExtension(updateUrl));
}
