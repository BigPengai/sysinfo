#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// config.yaml 示例：
// server:
//   ip: "192.168.1.100"
//   port: 1234
// api_path: "/api/system-info"   # 可选

std::string g_serverIp;
INTERNET_PORT g_serverPort = 0;
std::string g_apiPath = "/api/system-info";
std::string g_apiKey;
std::string g_baseDir;
typedef std::map<std::wstring, std::wstring> WmiRow;

std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Unquote(const std::string &s)
{
    std::string v = Trim(s);
    if (v.size() >= 2 && ((v[0] == '"' && v[v.size() - 1] == '"') ||
                          (v[0] == '\'' && v[v.size() - 1] == '\'')))
        return v.substr(1, v.size() - 2);
    return v;
}

std::string GetExeDirectory()
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return "";
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    if (p == std::string::npos)
        return ".";
    return s.substr(0, p);
}

std::string JoinPath(const std::string &dir, const char *name)
{
    if (dir.empty())
        return name;
    char c = dir[dir.size() - 1];
    return dir + ((c == '\\' || c == '/') ? "" : "\\") + name;
}

// Write one log line. Keep the log at or below 1 MiB by clearing it before
// an append that would exceed the limit. This works on older Windows APIs too.
void Log(const char *level, const std::string &message)
{
    if (g_baseDir.empty())
        g_baseDir = GetExeDirectory();
    if (g_baseDir.empty())
        return;
    const std::string path = JoinPath(g_baseDir, "GetSystemInfo.log");
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    sprintf(stamp, "%04u-%02u-%02u %02u:%02u:%02u",
            (unsigned)t.wYear, (unsigned)t.wMonth, (unsigned)t.wDay,
            (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond);
    std::string line = std::string("[") + stamp + "] [" + level + "] " + message + "\r\n";
    const unsigned long long maxSize = 1024ULL * 1024ULL;
    WIN32_FILE_ATTRIBUTE_DATA data;
    unsigned long long currentSize = 0;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    {
        currentSize = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    }
    // Truncate if already over the limit or this line would push it over.
    if (currentSize > maxSize || currentSize + line.size() > maxSize)
    {
        std::ofstream clearFile(path.c_str(), std::ios::binary | std::ios::trunc);
        if (clearFile)
        {
            clearFile << "[" << stamp << "] [WARN] 日志超过 1 MB，已自动清空旧内容\r\n";
            clearFile.close();
            currentSize = 0;
        }
    }
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::app);
    if (f)
        f.write(line.data(), (std::streamsize)line.size());
}

bool LoadConfig()
{
    g_baseDir = GetExeDirectory();
    if (g_baseDir.empty())
    {
        std::cerr << "Cannot determine executable directory.\n";
        Log("ERROR", "无法确定程序所在目录");
        return false;
    }
    std::ifstream f(JoinPath(g_baseDir, "config.yaml").c_str());
    if (!f)
    {
        std::cerr << "Cannot open config.yaml beside the EXE.\n";
        Log("ERROR", "无法打开 config.yaml");
        return false;
    }
    bool inServer = false;
    std::string line, ip, portText, api, apiKey;
    while (std::getline(f, line))
    {
        // Remove UTF-8 BOM if present on first line.
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        size_t hash = trimmed.find('#');
        if (hash != std::string::npos)
            trimmed = Trim(trimmed.substr(0, hash));
        size_t colon = trimmed.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = Trim(trimmed.substr(0, colon));
        std::string value = Unquote(trimmed.substr(colon + 1));
        bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        if (key == "server" && value.empty() && !indented)
        {
            inServer = true;
            continue;
        }
        if (!indented && key != "server")
            inServer = false;
        if (inServer && key == "ip")
            ip = value;
        else if (inServer && key == "port")
            portText = value;
        else if (key == "api_path")
            api = value;
        else if (key == "api_key")
            apiKey = value;
    }
    if (ip.empty() || portText.empty())
    {
        std::cerr << "config.yaml must contain server.ip and server.port.\n";
        Log("ERROR", "config.yaml 缺少 server.ip 或 server.port");
        return false;
    }
    if (apiKey.empty())
    {
        std::cerr << "config.yaml must contain api_key.\n";
        Log("ERROR", "config.yaml 缺少 api_key");
        return false;
    }
    unsigned int a, b, c, d;
    char extra;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
    {
        std::cerr << "Invalid IPv4 address in config.yaml.\n";
        Log("ERROR", "config.yaml 中的 IPv4 地址无效");
        return false;
    }
    char *end = NULL;
    unsigned long port = strtoul(portText.c_str(), &end, 10);
    if (portText.empty() || end == portText.c_str() || *end != '\0' || port < 1 || port > 65535)
    {
        std::cerr << "Invalid port in config.yaml.\n";
        Log("ERROR", "config.yaml 中的端口无效");
        return false;
    }
    if (!api.empty())
    {
        if (api[0] != '/')
        {
            std::cerr << "api_path must begin with /.\n";
            Log("ERROR", "api_path 必须以 / 开头");
            return false;
        }
        g_apiPath = api;
    }
    g_serverIp = ip;
    g_serverPort = (INTERNET_PORT)port;
    g_apiKey = apiKey;
    Log("INFO", std::string("配置读取成功，服务器=") + g_serverIp + ":" + portText + ", 接口=" + g_apiPath);
    return true;
}

std::string ToUtf8(const std::wstring &s)
{
    if (s.empty())
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 1)
        return "";
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, NULL, NULL);
    out.resize(n - 1); // exclude terminating NUL
    return out;
}

std::string JsonQuote(const std::wstring &w)
{
    std::string s = ToUtf8(w), out = "\"";
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char b[7];
                sprintf(b, "\\u%04x", (unsigned int)c);
                out += b;
            }
            else
                out += (char)c;
        }
    }
    out += "\"";
    return out;
}

std::wstring VariantToString(VARIANT &v)
{
    std::wstring out;
    if ((v.vt & VT_ARRAY) && v.parray)
    {
        SAFEARRAY *a = v.parray;
        LONG lo = 0, hi = -1;
        if (FAILED(SafeArrayGetLBound(a, 1, &lo)) || FAILED(SafeArrayGetUBound(a, 1, &hi)))
            return L"";
        for (LONG i = lo; i <= hi; ++i)
        {
            if ((v.vt & VT_TYPEMASK) == VT_BSTR)
            {
                BSTR item = NULL;
                if (SUCCEEDED(SafeArrayGetElement(a, &i, &item)) && item)
                {
                    if (!out.empty())
                        out += L",";
                    out += item;
                    SysFreeString(item);
                }
            }
        }
        return out;
    }
    VARIANT converted;
    VariantInit(&converted);
    if (SUCCEEDED(VariantChangeType(&converted, &v, 0, VT_BSTR)) && converted.bstrVal)
        out = converted.bstrVal;
    VariantClear(&converted);
    return out;
}

bool QueryWmi(IWbemServices *svc, const wchar_t *query,
              const std::vector<std::wstring> &fields, std::vector<WmiRow> &rows)
{
    rows.clear();
    IEnumWbemClassObject *en = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    if (!lang || !q)
    {
        if (lang)
            SysFreeString(lang);
        if (q)
            SysFreeString(q);
        return false;
    }
    HRESULT hr = svc->ExecQuery(lang, q,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !en)
        return false;
    while (true)
    {
        IWbemClassObject *obj = NULL;
        ULONG got = 0;
        hr = en->Next(WBEM_INFINITE, 1, &obj, &got);
        if (FAILED(hr) || got == 0 || !obj)
            break;
        WmiRow row;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            VARIANT val;
            VariantInit(&val);
            BSTR prop = SysAllocString(fields[i].c_str());
            if (prop)
            {
                if (SUCCEEDED(obj->Get(prop, 0, &val, NULL, NULL)))
                    row[fields[i]] = VariantToString(val);
                else
                    row[fields[i]] = L"";
                SysFreeString(prop);
            }
            else
                row[fields[i]] = L"";
            VariantClear(&val);
        }
        rows.push_back(row);
        obj->Release();
    }
    en->Release();
    return true;
}

std::wstring Field(const WmiRow &r, const wchar_t *name)
{
    WmiRow::const_iterator it = r.find(name);
    return it == r.end() ? L"" : it->second;
}

unsigned long long Number(const WmiRow &r, const wchar_t *name)
{
    std::wstring s = Field(r, name);
    if (s.empty())
        return 0;
    return wcstoull(s.c_str(), NULL, 10);
}

std::string Num(unsigned long long n)
{
    std::ostringstream o;
    o << n;
    return o.str();
}
std::string Percent(unsigned long long used, unsigned long long total)
{
    if (!total)
        return "0";
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << (100.0 * (double)used / (double)total);
    return o.str();
}

IWbemServices *ConnectWmi()
{
    IWbemLocator *locator = NULL;
    IWbemServices *svc = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (void **)&locator);
    if (FAILED(hr) || !locator)
        return NULL;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    if (!ns)
    {
        locator->Release();
        return NULL;
    }
    hr = locator->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(ns);
    locator->Release();
    if (FAILED(hr) || !svc)
        return NULL;
    hr = CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr))
    {
        svc->Release();
        return NULL;
    }
    return svc;
}

std::string CollectJson(IWbemServices *svc)
{
    std::vector<WmiRow> rows;
    std::wstring computer, username, cpu;
    unsigned long long totalMem = 0, freeMem = 0, logicalCpu = 0;
    if (QueryWmi(svc, L"SELECT Name, UserName, TotalPhysicalMemory FROM Win32_ComputerSystem",
                 std::vector<std::wstring>{L"Name", L"UserName", L"TotalPhysicalMemory"}, rows) &&
        !rows.empty())
    {
        computer = Field(rows[0], L"Name");
        username = Field(rows[0], L"UserName");
        totalMem = Number(rows[0], L"TotalPhysicalMemory");
    }
    std::vector<std::wstring> ips;
    if (QueryWmi(svc, L"SELECT IPAddress FROM Win32_NetworkAdapterConfiguration WHERE IPEnabled = TRUE",
                 std::vector<std::wstring>{L"IPAddress"}, rows))
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            std::wstring s = Field(rows[i], L"IPAddress");
            size_t start = 0;
            while (start < s.size())
            {
                size_t end = s.find(L',', start);
                std::wstring ip = s.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                if (ip.find(L'.') != std::wstring::npos && ip != L"127.0.0.1" && !ip.empty())
                {
                    bool found = false;
                    for (size_t j = 0; j < ips.size(); ++j)
                        if (ips[j] == ip)
                            found = true;
                    if (!found)
                        ips.push_back(ip);
                }
                if (end == std::wstring::npos)
                    break;
                start = end + 1;
            }
        }
    }
    if (QueryWmi(svc, L"SELECT Name, NumberOfLogicalProcessors, NumberOfCores FROM Win32_Processor",
                 std::vector<std::wstring>{L"Name", L"NumberOfLogicalProcessors", L"NumberOfCores"}, rows) &&
        !rows.empty())
    {
        cpu = Field(rows[0], L"Name");
        logicalCpu = Number(rows[0], L"NumberOfLogicalProcessors");
        if (!logicalCpu)
            logicalCpu = Number(rows[0], L"NumberOfCores");
    }
    if (QueryWmi(svc, L"SELECT FreePhysicalMemory FROM Win32_OperatingSystem",
                 std::vector<std::wstring>{L"FreePhysicalMemory"}, rows) &&
        !rows.empty())
        freeMem = Number(rows[0], L"FreePhysicalMemory") * 1024ULL;
    std::vector<WmiRow> physical, logical;
    QueryWmi(svc, L"SELECT Model, SerialNumber, Size FROM Win32_DiskDrive",
             std::vector<std::wstring>{L"Model", L"SerialNumber", L"Size"}, physical);
    QueryWmi(svc, L"SELECT DeviceID, Size, FreeSpace FROM Win32_LogicalDisk WHERE DriveType = 3",
             std::vector<std::wstring>{L"DeviceID", L"Size", L"FreeSpace"}, logical);

    std::ostringstream j;
    j << "{\"computer_name\":" << JsonQuote(computer) << ",\"computer\":{\"computerName\":" << JsonQuote(computer)
      << ",\"username\":" << JsonQuote(username) << ",\"ipAddresses\":[";
    for (size_t i = 0; i < ips.size(); ++i)
    {
        if (i)
            j << ",";
        j << JsonQuote(ips[i]);
    }
    j << "]},\"cpu\":{\"model\":" << JsonQuote(cpu) << ",\"logicalProcessorCount\":" << Num(logicalCpu) << "},";
    unsigned long long usedMem = totalMem > freeMem ? totalMem - freeMem : 0;
    j << "\"memory\":{\"totalBytes\":" << Num(totalMem) << ",\"availableBytes\":" << Num(freeMem)
      << ",\"usedPercent\":" << Percent(usedMem, totalMem) << "},\"physicalDisks\":[";
    for (size_t i = 0; i < physical.size(); ++i)
    {
        if (i)
            j << ",";
        j << "{\"model\":" << JsonQuote(Field(physical[i], L"Model")) << ",\"serialNumber\":" << JsonQuote(Field(physical[i], L"SerialNumber")) << ",\"sizeBytes\":" << Num(Number(physical[i], L"Size")) << "}";
    }
    j << "],\"logicalDisks\":[";
    for (size_t i = 0; i < logical.size(); ++i)
    {
        if (i)
            j << ",";
        unsigned long long t = Number(logical[i], L"Size"), f = Number(logical[i], L"FreeSpace"), u = t > f ? t - f : 0;
        j << "{\"drive\":" << JsonQuote(Field(logical[i], L"DeviceID")) << ",\"totalBytes\":" << Num(t) << ",\"freeBytes\":" << Num(f) << ",\"usedPercent\":" << Percent(u, t) << "}";
    }
    j << "]}";
    return j.str();
}

bool PostJson(const std::string &json)
{
    HINTERNET internet = InternetOpenA("SystemInfoCollector", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!internet)
    {
        std::cerr << "InternetOpen failed: " << GetLastError() << "\n";
        Log("ERROR", std::string("InternetOpen 失败，错误码=") + Num(GetLastError()));
        return false;
    }
    DWORD timeout = 15000;
    InternetSetOptionA(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(internet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    HINTERNET conn = InternetConnectA(internet, g_serverIp.c_str(), g_serverPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn)
    {
        DWORD e = GetLastError();
        std::cerr << "InternetConnect failed: " << e << "\n";
        Log("ERROR", std::string("连接服务器失败，错误码=") + Num(e));
        InternetCloseHandle(internet);
        return false;
    }
    HINTERNET req = HttpOpenRequestA(conn, "POST", g_apiPath.c_str(), "HTTP/1.1", NULL, NULL, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!req)
    {
        DWORD e = GetLastError();
        std::cerr << "HttpOpenRequest failed: " << e << "\n";
        Log("ERROR", std::string("创建 HTTP 请求失败，错误码=") + Num(e));
        InternetCloseHandle(conn);
        InternetCloseHandle(internet);
        return false;
    }
    std::string headers = "Content-Type: application/json; charset=utf-8\r\nX-API-Key: " + g_apiKey + "\r\n";
    Log("INFO", std::string("开始 HTTP 上传，目标=") + g_serverIp + ":" + Num(g_serverPort));
    BOOL ok = HttpSendRequestA(req, headers.c_str(), (DWORD)headers.size(), (LPVOID)json.data(), (DWORD)json.size());
    bool success = false;
    if (ok)
    {
        DWORD status = 0, size = sizeof(status);
        if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL))
        {
            std::cout << "HTTP status: " << status << "\n";
            success = status >= 200 && status < 300;
            Log(success ? "INFO" : "ERROR", std::string("服务器 HTTP 状态码=") + Num(status));
        }
        else
        {
            Log("ERROR", "无法读取服务器 HTTP 状态码");
        }
    }
    else
    {
        DWORD e = GetLastError();
        std::cerr << "HttpSendRequest failed: " << e << "\n";
        Log("ERROR", std::string("HTTP 上传请求失败，错误码=") + Num(e));
    }
    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(internet);
    return success;
}

std::string Today()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    sprintf(b, "%04u-%02u-%02u", (unsigned)t.wYear, (unsigned)t.wMonth, (unsigned)t.wDay);
    return b;
}

bool AlreadyUploadedToday()
{
    std::ifstream f(JoinPath(g_baseDir, "last-success-date.txt").c_str());
    std::string saved;
    if (f)
        std::getline(f, saved);
    return Trim(saved) == Today();
}

bool SaveUploadDate()
{
    std::ofstream f(JoinPath(g_baseDir, "last-success-date.txt").c_str(), std::ios::binary);
    if (!f)
        return false;
    f << Today() << "\n";
    f.close();
    return !f.fail();
}

int main()
{
    g_baseDir = GetExeDirectory();
    Log("INFO", "========== 程序启动 ==========");
    if (!LoadConfig())
    {
        Log("ERROR", "程序结束：配置读取失败");
        return 10;
    }
    if (AlreadyUploadedToday())
    {
        std::cout << "Already uploaded successfully today; skipping.\n";
        Log("INFO", "今天已成功上传，跳过本次执行");
        Log("INFO", "========== 程序结束 ==========");
        return 0;
    }
    Log("INFO", "开始初始化 COM/WMI");
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        std::cerr << "COM initialization failed: " << std::hex << hr << "\n";
        Log("ERROR", "COM 初始化失败");
        return 11;
    }
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
    {
        std::cerr << "COM security initialization failed.\n";
        Log("ERROR", "COM 安全初始化失败");
        CoUninitialize();
        return 12;
    }
    IWbemServices *svc = ConnectWmi();
    if (!svc)
    {
        std::cerr << "Cannot connect to WMI ROOT\\CIMV2.\n";
        Log("ERROR", "无法连接 WMI ROOT\\CIMV2");
        CoUninitialize();
        return 13;
    }
    Log("INFO", "WMI 连接成功，开始采集系统信息");
    std::string json = CollectJson(svc);
    svc->Release();
    CoUninitialize();
    if (json.empty())
    {
        std::cerr << "Failed to collect system information.\n";
        Log("ERROR", "系统信息采集失败，JSON 内容为空");
        return 14;
    }
    Log("INFO", std::string("系统信息采集完成，JSON 字节数=") + Num(json.size()));
    std::string jsonPath = JoinPath(g_baseDir, "system-info.json");
    std::ofstream file(jsonPath.c_str(), std::ios::binary);
    if (!file)
    {
        std::cerr << "Cannot write " << jsonPath << "\n";
        Log("ERROR", std::string("无法创建 JSON 文件：") + jsonPath);
        return 15;
    }
    file.write(json.data(), (std::streamsize)json.size());
    file.close();
    if (file.fail())
    {
        std::cerr << "Failed writing JSON file.\n";
        Log("ERROR", "写入 system-info.json 失败");
        return 16;
    }
    std::cout << "JSON saved: " << jsonPath << "\n";
    Log("INFO", std::string("JSON 文件保存成功：") + jsonPath);
    if (!PostJson(json))
    {
        std::cerr << "HTTP upload failed.\n";
        Log("ERROR", "HTTP 上传失败，本次不更新每日成功标记");
        return 17;
    }
    if (!SaveUploadDate())
    {
        std::cerr << "Upload succeeded but could not save daily marker; next run may upload again.\n";
        Log("ERROR", "上传成功，但保存每日成功标记失败");
        return 18;
    }
    std::cout << "Upload succeeded.\n";
    Log("INFO", "HTTP 上传成功，每日成功标记已保存");
    Log("INFO", "========== 程序结束 ==========");
    return 0;
}
