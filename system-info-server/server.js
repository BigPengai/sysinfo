
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');
const YAML = require('yaml');

// 读取 server.js 同目录下的 server_config.yaml
const CONFIG_FILE = path.join(__dirname, 'server_config.yaml');

if (!fs.existsSync(CONFIG_FILE)) {
  console.error(`配置文件不存在：${CONFIG_FILE}`);
  process.exit(1);
}

let config;

try {
  const configText = fs.readFileSync(CONFIG_FILE, 'utf8');
  config = YAML.parse(configText);
} catch (err) {
  console.error(`读取或解析 config.yaml 失败：${err.message}`);
  process.exit(1);
}

const PORT = Number(config?.server?.port ?? 1234);
const API_KEY = config?.server?.api_key;

if (!Number.isInteger(PORT) || PORT < 1 || PORT > 65535) {
  console.error('config.yaml 中的 server.port 配置无效');
  process.exit(1);
}

if (typeof API_KEY !== 'string' || API_KEY.trim().length < 16) {
  console.error('config.yaml 中的 server.api_key 无效，至少需要 16 个字符');
  process.exit(1);
}

console.log(`配置文件加载成功，监听端口：${PORT}`);

const MAX_BODY_SIZE = 1024 * 1024; // 1 MB
const MAX_LOG_SIZE = 1024 * 1024;  // 1 MB

const ROOT_DIR = __dirname;
const DATA_DIR = path.join(ROOT_DIR, 'data');
const LOG_DIR = path.join(ROOT_DIR, 'logs');
const LOG_FILE = path.join(LOG_DIR, 'server.log');
const DB_FILE = path.join(DATA_DIR, 'system-info.db');

fs.mkdirSync(DATA_DIR, { recursive: true });
fs.mkdirSync(LOG_DIR, { recursive: true });

// 日志超过 1 MB 时清空旧内容
function log(level, message) {
  try {
    const line =
      `[${new Date().toISOString()}] [${level}] ${message}\n`;

    if (
      fs.existsSync(LOG_FILE) &&
      fs.statSync(LOG_FILE).size + Buffer.byteLength(line, 'utf8') >
      MAX_LOG_SIZE
    ) {
      fs.writeFileSync(LOG_FILE, '');
    }

    fs.appendFileSync(LOG_FILE, line, 'utf8');
  } catch (err) {
    console.error('写入日志失败:', err.message);
  }

  console.log(`[${level}] ${message}`);
}

if (!API_KEY) {
  console.error(
    '缺少 SYSTEM_INFO_API_KEY 环境变量，请先设置 API 密钥。'
  );
  process.exit(1);
}

const db = new DatabaseSync(DB_FILE);

db.exec(`
  PRAGMA journal_mode = WAL;
  PRAGMA busy_timeout = 5000;

  CREATE TABLE IF NOT EXISTS computers (
    computer_name TEXT PRIMARY KEY,
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL
  );

  CREATE TABLE IF NOT EXISTS system_reports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    computer_name TEXT NOT NULL,
    report_date TEXT NOT NULL,
    received_at TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    UNIQUE(computer_name, report_date),
    FOREIGN KEY (computer_name)
      REFERENCES computers(computer_name)
  );

  CREATE INDEX IF NOT EXISTS idx_reports_name_date
  ON system_reports(computer_name, report_date DESC);
`);

const findComputer = db.prepare(`
  SELECT computer_name FROM computers
  WHERE computer_name = ?
`);

const insertComputer = db.prepare(`
  INSERT INTO computers(computer_name, first_seen, last_seen)
  VALUES (?, ?, ?)
  ON CONFLICT(computer_name)
  DO UPDATE SET last_seen = excluded.last_seen
`);

const saveReport = db.prepare(`
  INSERT INTO system_reports
    (computer_name, report_date, received_at, payload_json)
  VALUES (?, ?, ?, ?)
  ON CONFLICT(computer_name, report_date)
  DO UPDATE SET
    received_at = excluded.received_at,
    payload_json = excluded.payload_json
`);

const listComputers = db.prepare(`
  SELECT
    c.computer_name,
    c.first_seen,
    c.last_seen,
    COUNT(r.id) AS report_count
  FROM computers c
  LEFT JOIN system_reports r
    ON r.computer_name = c.computer_name
  GROUP BY c.computer_name
  ORDER BY c.computer_name
`);

const getHistory = db.prepare(`
  SELECT id, computer_name, report_date, received_at, payload_json
  FROM system_reports
  WHERE computer_name = ?
  ORDER BY report_date DESC
  LIMIT 3650
`);

const getRecentReports = db.prepare(`
  SELECT id, computer_name, report_date, received_at, payload_json
  FROM system_reports
  ORDER BY received_at DESC
  LIMIT 100
`);

function sendJson(res, statusCode, data) {
  const body = JSON.stringify(data);

  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store'
  });

  res.end(body);
}

function isAuthorized(req) {
  const supplied = req.headers['x-api-key'];

  // 使用固定长度 Buffer，避免直接比较密钥时泄露长度信息。
  if (typeof supplied !== 'string') return false;

  const a = Buffer.from(supplied);
  const b = Buffer.from(API_KEY);

  return a.length === b.length && require('node:crypto')
    .timingSafeEqual(a, b);
}


function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    let tooLarge = false;
    const chunks = [];

    req.on('data', chunk => {
      size += chunk.length;

      if (size > MAX_BODY_SIZE) {
        tooLarge = true;
        chunks.length = 0;
        return;
      }

      if (!tooLarge) {
        chunks.push(chunk);
      }
    });

    req.on('end', () => {
      if (tooLarge) {
        return reject(new Error('BODY_TOO_LARGE'));
      }

      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));
      } catch {
        reject(new Error('INVALID_JSON'));
      }
    });

    req.on('error', reject);
  });
}


function getComputerName(payload) {
  return payload.computer_name ??
    payload.computerName ??
    payload.hostname ??
    null;
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);

  // 所有接口均要求 API 密钥
  if (!isAuthorized(req)) {
    log('WARN', `认证失败：${req.method} ${url.pathname}`);
    return sendJson(res, 401, { error: 'Unauthorized' });
  }

  if (req.method === 'GET' && url.pathname === '/api/health') {
    return sendJson(res, 200, { status: 'ok' });
  }

  if (req.method === 'POST' && url.pathname === '/api/system-info') {
    try {
      const payload = await readJsonBody(req);

      if (
        payload === null ||
        typeof payload !== 'object' ||
        Array.isArray(payload)
      ) {
        return sendJson(res, 400, { error: 'JSON 必须是对象' });
      }

      const name = getComputerName(payload);

      if (
        typeof name !== 'string' ||
        name.trim().length === 0 ||
        name.length > 255
      ) {
        return sendJson(res, 400, {
          error: '缺少有效的 computer_name 字段'
        });
      }

      const computerName = name.trim();
      const receivedAt = new Date().toISOString();
      const reportDate = receivedAt.slice(0, 10);

      // 事务保证计算机登记和快照写入同时成功或同时回滚
      db.exec('BEGIN');

      try {
        insertComputer.run(computerName, receivedAt, receivedAt);
        saveReport.run(
          computerName,
          reportDate,
          receivedAt,
          JSON.stringify(payload)
        );
        db.exec('COMMIT');
      } catch (err) {
        db.exec('ROLLBACK');
        throw err;
      }

      log('INFO', `上报保存成功：${computerName}, 日期=${reportDate}`);

      return sendJson(res, 200, {
        success: true,
        computer_name: computerName,
        report_date: reportDate,
        received_at: receivedAt
      });
    } catch (err) {
      if (err.message === 'BODY_TOO_LARGE') {
        return sendJson(res, 413, { error: '请求体不能超过 1 MB' });
      }

      if (err.message === 'INVALID_JSON') {
        return sendJson(res, 400, { error: 'JSON 格式错误' });
      }

      log('ERROR', `保存上报失败：${err.message}`);
      return sendJson(res, 500, { error: '服务器内部错误' });
    }
  }

  if (req.method === 'GET' && url.pathname === '/api/computers') {
    return sendJson(res, 200, listComputers.all());
  }

  if (req.method === 'GET' && url.pathname === '/api/system-info') {
    const name = url.searchParams.get('computer_name');

    if (name) {
      const rows = getHistory.all(name);
      return sendJson(res, 200, rows.map(row => ({
        ...row,
        data: JSON.parse(row.payload_json)
      })));
    }

    return sendJson(res, 200, getRecentReports.all().map(row => ({
      ...row,
      data: JSON.parse(row.payload_json)
    })));
  }

  return sendJson(res, 404, { error: 'Not Found' });
});

server.listen(PORT, '0.0.0.0', () => {
  log('INFO', `服务已启动，监听端口 ${PORT}`);
});

function shutdown() {
  log('INFO', '服务正在关闭');
  server.close(() => {
    db.close();
    process.exit(0);
  });
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
