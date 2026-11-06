import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";
export function db() {
  const sql = new DatabaseSync(":memory:");
  sql.exec(readFileSync("migrations/0001_portal.sql", "utf8"));
  sql.exec(
    "INSERT INTO tenants VALUES('a','A'),('b','B'); INSERT INTO members(email,tenant_id,role) VALUES('admin@a.test','a','admin'),('member@a.test','a','member'),('member@b.test','b','member');",
  );
  const prepare = (query: string) => {
    let args: any[] = [];
    return {
      bind(...values: any[]) {
        args = values;
        return this;
      },
      async first() {
        return sql.prepare(query).get(...args) ?? null;
      },
      async all() {
        return { results: sql.prepare(query).all(...args) };
      },
      async run() {
        return sql.prepare(query).run(...args);
      },
    };
  };
  return { prepare };
}
