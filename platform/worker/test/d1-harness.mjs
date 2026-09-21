import { readFileSync } from 'node:fs';
import { DatabaseSync } from 'node:sqlite';

class D1Statement {
  constructor(owner, sql, values = []) { Object.assign(this, { owner, sql, values }); }
  bind(...values) { return new D1Statement(this.owner, this.sql, values); }
  native() { return this.owner.database.prepare(this.sql); }
  async first(column) {
    const row = this.native().get(...this.values);
    if (!row) return null;
    return column === undefined ? { ...row } : row[column];
  }
  async all() {
    const results = this.native().all(...this.values).map(row => ({ ...row }));
    return { success: true, results, meta: { changes: 0 } };
  }
  async run() { return this.executeSync(); }
  executeSync() {
    const statement = this.native();
    if (statement.columns().length) {
      return { success: true, results: statement.all(...this.values).map(row => ({ ...row })), meta: { changes: 0 } };
    }
    const result = statement.run(...this.values);
    return { success: true, results: [], meta: { changes: Number(result.changes), last_row_id: Number(result.lastInsertRowid) } };
  }
}

/** Real SQLite SQL/constraints. No await occurs inside a batch transaction. */
export class D1Harness {
  constructor({ migrate = true } = {}) {
    this.database = new DatabaseSync(':memory:');
    if (migrate) this.exec(readFileSync(new URL('../migrations/0001_accounts.sql', import.meta.url), 'utf8'));
  }
  prepare(sql) { return new D1Statement(this, sql); }
  exec(sql) { this.database.exec(sql); }
  async batch(statements) {
    this.database.exec('BEGIN IMMEDIATE');
    try {
      const results = statements.map(statement => {
        if (!(statement instanceof D1Statement) || statement.owner !== this) throw new Error('Foreign statement in D1 batch');
        return statement.executeSync();
      });
      this.database.exec('COMMIT');
      return results;
    } catch (error) {
      this.database.exec('ROLLBACK');
      throw error;
    }
  }
  close() { this.database.close(); }
}
