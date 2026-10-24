
const express = require('express');
const path = require('node:path');
const fs = require('node:fs');
const { DatabaseSync } = require('node:sqlite');

const app = express();
const PORT = 4321;

// 与 server.js 使用同一个 SQLite 数据库文件
const DB_FILE = path.join(__dirname, 'data', 'system-info.db');

if (!fs.existsSync(DB_FILE)) {
    console.error(`数据库不存在：${DB_FILE}`);
    console.error('请先启动 server.js，创建并初始化数据库。');
    process.exit(1);
}

const db = new DatabaseSync(DB_FILE);

db.exec(`
  PRAGMA busy_timeout = 5000;

  CREATE TABLE IF NOT EXISTS computers_info (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    report_id INTEGER NOT NULL UNIQUE,
    computer_name TEXT NOT NULL,
    report_date TEXT NOT NULL,
    received_at TEXT,
    username TEXT,
    ip_addresses_json TEXT,
    cpu_model TEXT,
    logical_processors INTEGER,
    total_memory_mb REAL,
    available_memory_mb REAL,
    physical_disks_json TEXT,
    logical_disks_json TEXT,
    payload_json TEXT NOT NULL,
    synced_at TEXT NOT NULL
  );

  CREATE INDEX IF NOT EXISTS idx_computers_info_name_date
  ON computers_info(computer_name, report_date DESC);
`);

const upsertInfo = db.prepare(`
  INSERT INTO computers_info (
    report_id,
    computer_name,
    report_date,
    received_at,
    username,
    ip_addresses_json,
    cpu_model,
    logical_processors,
    total_memory_mb,
    available_memory_mb,
    physical_disks_json,
    logical_disks_json,
    payload_json,
    synced_at
  ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  ON CONFLICT(report_id) DO UPDATE SET
    computer_name = excluded.computer_name,
    report_date = excluded.report_date,
    received_at = excluded.received_at,
    username = excluded.username,
    ip_addresses_json = excluded.ip_addresses_json,
    cpu_model = excluded.cpu_model,
    logical_processors = excluded.logical_processors,
    total_memory_mb = excluded.total_memory_mb,
    available_memory_mb = excluded.available_memory_mb,
    physical_disks_json = excluded.physical_disks_json,
    logical_disks_json = excluded.logical_disks_json,
    payload_json = excluded.payload_json,
    synced_at = excluded.synced_at
`);

function firstValue(...values) {
    return values.find(v => v !== undefined && v !== null && v !== '');
}

function toSqlNumber(value) {
    if (value === null || value === undefined || value === '') {
        return null;
    }

    // SQLite 的数值字段不应接收对象、数组或布尔值
    if (
        typeof value !== 'number' &&
        typeof value !== 'string'
    ) {
        return null;
    }

    const number = Number(value);

    return Number.isFinite(number) ? number : null;
}

function jsonText(value) {
    return JSON.stringify(value ?? []);
}

function parsePayload(row) {
    const payload = JSON.parse(row.payload_json);

    const computer = payload.computer ?? {};
    const cpu = payload.cpu ?? {};
    const memory = payload.memory ?? {};

    const computerName = firstValue(
        payload.computer_name,
        payload.computerName,
        payload.hostname,
        computer.computerName,
        computer.name,
        row.computer_name
    );

    if (typeof computerName !== 'string' || !computerName.trim()) {
        throw new Error(`上报记录 ${row.id} 缺少计算机名`);
    }

    const ipAddresses = firstValue(
        payload.ip_addresses,
        payload.ipAddresses,
        computer.ipAddresses,
        computer.ip_addresses,
        []
    );

    const physicalDisks = firstValue(
        payload.physicalDisks,
        payload.physical_disks,
        payload.disks?.physical,
        []
    );

    const logicalDisks = firstValue(
        payload.logicalDisks,
        payload.logical_disks,
        payload.disks?.logical,
        []
    );

    return {
        reportId: row.id,
        computerName: computerName.trim(),
        reportDate: row.report_date || row.received_at?.slice(0, 10) ||
            new Date().toISOString().slice(0, 10),
        receivedAt: row.received_at ?? null,
        username: firstValue(
            payload.username,
            payload.userName,
            computer.username,
            computer.userName,
            null
        ),
        ipAddresses,
        cpuModel: firstValue(
            cpu.model,
            cpu.cpuModel,
            cpu.name,
            payload.cpu_model,
            null
        ),
        logicalProcessors: toSqlNumber(firstValue(
            cpu.logical_processors,
            cpu.logicalProcessors,
            cpu.numberOfLogicalProcessors,
            null
        )),
        totalMemoryMb: toSqlNumber(firstValue(
        memory.total_mb,
        memory.totalMB,
        memory.totalMemoryMB,
        null
        )),
        availableMemoryMb: toSqlNumber(firstValue(
        memory.available_mb,
        memory.availableMB,
        memory.availableMemoryMB,
        null
        )),
        physicalDisks,
        logicalDisks,
        payloadJson: row.payload_json
    };
}

// 将 system_reports 中的上报记录同步到 computers_info。
// report_id 唯一，重复同步会更新而不会重复插入。
function syncReports() {
    const rows = db.prepare(`
    SELECT id, computer_name, report_date, received_at, payload_json
    FROM system_reports
    ORDER BY id
  `).all();

    let synced = 0;
    let failed = 0;

    db.exec('BEGIN');

    try {
        for (const row of rows) {
            try {
                const item = parsePayload(row);

                upsertInfo.run(
                    item.reportId,
                    item.computerName,
                    item.reportDate,
                    item.receivedAt,
                    item.username,
                    jsonText(item.ipAddresses),
                    item.cpuModel,
                    item.logicalProcessors,
                    item.totalMemoryMb,
                    item.availableMemoryMb,
                    jsonText(item.physicalDisks),
                    jsonText(item.logicalDisks),
                    item.payloadJson,
                    new Date().toISOString()
                );

                synced++;
            } catch (err) {
                failed++;
                console.error(`跳过记录 id=${row.id}：${err.message}`);
            }
        }

        db.exec('COMMIT');
    } catch (err) {
        db.exec('ROLLBACK');
        throw err;
    }

    return { total: rows.length, synced, failed };
}

// HTML 输出必须转义，避免客户端上报内容被当成 HTML 执行。
function escapeHtml(value) {
    return String(value ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}

// 将嵌套 JSON 转换为适合表格显示的字段
function flattenObject(obj, prefix = '', result = {}) {
    if (obj === null || obj === undefined) {
        if (prefix) result[prefix] = '';
        return result;
    }

    if (Array.isArray(obj)) {
        // 数组可能包含多个 IP、物理磁盘或逻辑磁盘
        result[prefix || '数据'] = JSON.stringify(obj);
        return result;
    }

    if (typeof obj !== 'object') {
        if (prefix) result[prefix] = String(obj);
        return result;
    }

    for (const [key, value] of Object.entries(obj)) {
        const fieldName = prefix ? `${prefix}.${key}` : key;

        if (value !== null && typeof value === 'object'
            && !Array.isArray(value)) {
            flattenObject(value, fieldName, result);
        } else if (Array.isArray(value)) {
            result[fieldName] = JSON.stringify(value);
        } else {
            result[fieldName] =
                value === null || value === undefined
                    ? ''
                    : String(value);
        }
    }

    return result;
}

function formatJson(value) {
    try {
        return JSON.stringify(JSON.parse(value || '[]'), null, 2);
    } catch {
        return String(value ?? '');
    }
}

function page(title, body) {
    return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(title)}</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 24px; color: #222; }
    h1 { font-size: 24px; }
    a { color: #1769aa; text-decoration: none; }
    a:hover { text-decoration: underline; }
    .toolbar { margin: 16px 0; display: flex; gap: 16px; flex-wrap: wrap; }
    .button { display: inline-block; padding: 8px 12px; border: 1px solid #bbb;
      border-radius: 5px; background: #f7f7f7; cursor: pointer; }
    table { width: 100%; border-collapse: collapse; margin-top: 16px; }
    th, td { border: 1px solid #ddd; padding: 9px; text-align: left;
      vertical-align: top; }
    th { background: #f2f4f7; }
    tr:nth-child(even) { background: #fafafa; }
    pre { white-space: pre-wrap; overflow-wrap: anywhere; background: #f5f5f5;
      padding: 12px; border-radius: 5px; }
    .muted { color: #666; }
  </style>
</head>
<body>
${body}
</body>
</html>`;
}

app.get('/', (req, res) => {
    try {
        // 获取每台计算机最新的一份报告
        const reports = db.prepare(`
            SELECT
                computer_name,
                report_date,
                received_at,
                payload_json
            FROM system_reports
            WHERE id IN (
                SELECT MAX(id)
                FROM system_reports
                GROUP BY computer_name
            )
            ORDER BY computer_name
        `).all();

        // 解析每台计算机的完整 JSON
        const rows = reports.map(report => {
            let payload;

            try {
                payload = JSON.parse(report.payload_json);
            } catch (error) {
                payload = {
                    json_parse_error: error.message,
                    raw_payload: report.payload_json
                };
            }

            // 将采集时间也作为表格字段显示
            const fields = flattenObject(payload);

            fields['报告日期'] = report.report_date;
            fields['服务器接收时间'] = report.received_at;

            // 如果 JSON 中没有计算机名，使用数据库字段
            if (!fields.computer_name) {
                fields.computer_name = report.computer_name;
            }

            return fields;
        });

        // 汇总所有报告中的字段，自动生成表头
        const columns = [
            ...new Set(rows.flatMap(row => Object.keys(row)))
        ];

        const headerHtml = columns.map(column =>
            `<th>${escapeHtml(column)}</th>`
        ).join('');

        const bodyHtml = rows.map(row => {
            const cells = columns.map(column => {
                const value = row[column] ?? '';

                return `<td>${escapeHtml(value)}</td>`;
            }).join('');

            return `<tr>${cells}</tr>`;
        }).join('');

        res.send(`
        <!DOCTYPE html>
        <html lang="zh-CN">
        <head>
            <meta charset="UTF-8">
            <meta name="viewport"
                  content="width=device-width, initial-scale=1">
            <title>计算机信息总览</title>

            <style>
                * {
                    box-sizing: border-box;
                }

                body {
                    font-family: "Microsoft YaHei", Arial, sans-serif;
                    margin: 0;
                    padding: 24px;
                    background: #f4f6f9;
                    color: #202938;
                }

                h1 {
                    margin-bottom: 8px;
                }

                .summary {
                    margin-bottom: 20px;
                    color: #596579;
                }

                .table-container {
                    overflow: auto;
                    max-width: 100%;
                    max-height: 75vh;
                    background: white;
                    border: 1px solid #dce2ea;
                    border-radius: 8px;
                }

                table {
                    border-collapse: separate;
                    border-spacing: 0;
                    width: max-content;
                    min-width: 100%;
                    font-size: 13px;
                }

                th, td {
                    border-right: 1px solid #e3e8ef;
                    border-bottom: 1px solid #e3e8ef;
                    padding: 10px 12px;
                    text-align: left;
                    vertical-align: top;
                    max-width: 360px;
                    overflow-wrap: anywhere;
                    white-space: pre-wrap;
                }

                th {
                    position: sticky;
                    top: 0;
                    z-index: 1;
                    background: #eaf0f8;
                    white-space: nowrap;
                }

                tbody tr:nth-child(even) {
                    background: #f8fafc;
                }

                tbody tr:hover {
                    background: #edf5ff;
                }

                .empty {
                    padding: 30px;
                    text-align: center;
                    color: #687386;
                }
            </style>
        </head>

        <body>
            <h1>计算机信息总览</h1>

            <div class="summary">
                当前计算机数量：${rows.length}
                <span>；每台计算机显示最新报告</span>
            </div>

            <div class="table-container">
                ${
                    rows.length === 0
                    ? '<div class="empty">暂无采集数据</div>'
                    : `
                        <table>
                            <thead>
                                <tr>${headerHtml}</tr>
                            </thead>
                            <tbody>
                                ${bodyHtml}
                            </tbody>
                        </table>
                    `
                }
            </div>
        </body>
        </html>
        `);
    } catch (error) {
        console.error('首页查询失败：', error);
        res.status(500).send('首页查询失败，请查看服务器日志。');
    }
});

// 计算机详情页：显示历史记录以及解析后的系统信息
app.get('/computer/:name', (req, res) => {
    try {
        syncReports();

        const rows = db.prepare(`
      SELECT *
      FROM computers_info
      WHERE computer_name = ?
      ORDER BY report_date DESC, id DESC
    `).all(req.params.name);

        if (rows.length === 0) {
            return res.status(404).send('没有找到该计算机的记录。');
        }

        const historyRows = rows.map(row => {
            const payload = formatJson(row.payload_json);

            return `
        <tr>
          <td>${escapeHtml(row.report_date)}</td>
          <td>${escapeHtml(row.received_at)}</td>
          <td>${escapeHtml(row.username)}</td>
          <td>${escapeHtml(row.cpu_model)}</td>
          <td>${escapeHtml(row.logical_processors)}</td>
          <td>${escapeHtml(row.total_memory_mb)}</td>
          <td><details><summary>查看原始 JSON</summary>
            <pre>${escapeHtml(payload)}</pre>
          </details></td>
        </tr>
      `;
        }).join('');

        const first = rows[0];

        const body = `
      <p><a href="/">← 返回计算机列表</a></p>
      <h1>${escapeHtml(req.params.name)}</h1>
      <p>历史记录：${rows.length} 条</p>
      <h2>历史上报信息</h2>
      <table>
        <thead>
          <tr>
            <th>上报日期</th>
            <th>接收时间</th>
            <th>用户名</th>
            <th>CPU 型号</th>
            <th>逻辑处理器数</th>
            <th>总内存（MB）</th>
            <th>完整 JSON</th>
          </tr>
        </thead>
        <tbody>${historyRows}</tbody>
      </table>
      <h2>最新记录的 IP 地址</h2>
      <pre>${escapeHtml(formatJson(first.ip_addresses_json))}</pre>
      <h2>最新记录的物理磁盘</h2>
      <pre>${escapeHtml(formatJson(first.physical_disks_json))}</pre>
      <h2>最新记录的逻辑磁盘</h2>
      <pre>${escapeHtml(formatJson(first.logical_disks_json))}</pre>
    `;

        res.type('html').send(page(`${req.params.name} - 系统信息`, body));
    } catch (err) {
        console.error('详情查询失败：', err);
        res.status(500).send('服务器内部错误，请检查 web.js 所在终端的错误信息。');
    }
});

app.listen(PORT, '0.0.0.0', () => {
    console.log(`Web 管理页面已启动：http://localhost:${PORT}`);
    console.log(`局域网访问地址：http://服务器IP:${PORT}`);
});

function shutdown() {
    try {
        db.close();
    } finally {
        process.exit(0);
    }
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
