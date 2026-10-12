import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, symlink, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..')
const formatScript = 'gateway/ui/scripts/format.mjs'

async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'osk-format-'))
  t.after(() => rm(root, { recursive: true, force: true }))
  await mkdir(path.join(root, 'gateway/ui/scripts'), { recursive: true })
  await mkdir(path.join(root, '.githooks'))

  for (const file of [
    '.editorconfig',
    '.prettierrc.json',
    '.prettierignore',
    '.clang-format',
    '.githooks/pre-commit',
    formatScript,
  ])
    await copyFile(path.join(repositoryRoot, file), path.join(root, file))

  await chmod(path.join(root, '.githooks/pre-commit'), 0o755)

  await symlink(
    path.join(repositoryRoot, 'gateway/ui/node_modules'),
    path.join(root, 'gateway/ui/node_modules'),
    'junction',
  )
  const git = (...args) =>
    execFileSync('git', args, { cwd: root, encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] })
  git('init', '-q')
  git('config', 'user.name', 'Formatter test')
  git('config', 'user.email', 'formatter@example.invalid')
  git('config', 'core.autocrlf', 'false')
  await writeFile(path.join(root, '.gitignore'), 'gateway/ui/node_modules/\n')
  git('add', '.')
  git('-c', 'core.hooksPath=', 'commit', '-qm', 'Fixture')
  git('config', 'core.hooksPath', '.githooks')

  return { root, git }
}

test('C++ uses expanded blocks and a separate line for a single unbraced statement', async (t) => {
  const { root } = await fixture(t)
  const file = path.join(root, 'example.cpp')
  await writeFile(
    file,
    'bool read(int size) { if (size == 0) { return false; } if (size > 8) { size = 8; size--; } return true; }\n',
  )
  execFileSync(process.execPath, [formatScript, '--write', file], { cwd: root })
  const formatted = await readFile(file, 'utf8')
  assert.match(formatted, /if \(size == 0\)\n {8}return false;/)
  assert.match(formatted, /if \(size > 8\) \{\n {8}size = 8;\n {8}size--;/)
  execFileSync(process.execPath, [formatScript, '--check', file], { cwd: root })
})

test('pre-commit formats a filename with spaces and preserves partially staged edits', async (t) => {
  const { root, git } = await fixture(t)
  const file = 'sample file.ts'
  const padding = Array.from({ length: 15 }, (_, index) => `// separate section ${index}\n`).join(
    '',
  )
  const base = `export const staged = 1\n\n${padding}\nexport const unstaged = 1\n`
  await writeFile(path.join(root, file), base)
  git('add', file)
  git('commit', '-qm', 'Base')
  await writeFile(path.join(root, file), base.replace('staged = 1', 'staged={value:2};'))
  git('add', file)
  await writeFile(
    path.join(root, file),
    (await readFile(path.join(root, file), 'utf8')).replace('unstaged = 1', 'unstaged = 99'),
  )
  git('commit', '-qm', 'Formatted')
  const committed = git('show', `HEAD:${file}`)
  const working = await readFile(path.join(root, file), 'utf8')
  assert.match(committed, /export const staged = \{ value: 2 \}/)
  assert.match(committed, /export const unstaged = 1/)
  assert.match(working, /export const unstaged = 99/)
  assert.equal(git('diff', '--cached'), '')
})

test('formatter failure stops the commit and restores both index and unstaged edits', async (t) => {
  const { root, git } = await fixture(t)
  await writeFile(path.join(root, 'good.ts'), 'export const good={value:1};\n')
  await writeFile(path.join(root, 'bad.ts'), 'export const bad = {\n')
  git('add', 'good.ts', 'bad.ts')
  await writeFile(path.join(root, 'good.ts'), 'export const good={value:99};\n')
  const indexBefore = git('diff', '--cached')
  const workingBefore = await readFile(path.join(root, 'good.ts'), 'utf8')
  const result = spawnSync('git', ['commit', '-qm', 'Must fail'], { cwd: root, encoding: 'utf8' })
  assert.notEqual(result.status, 0)
  assert.equal(git('diff', '--cached'), indexBefore)
  assert.equal(await readFile(path.join(root, 'good.ts'), 'utf8'), workingBefore)
  assert.equal(await readFile(path.join(root, 'bad.ts'), 'utf8'), 'export const bad = {\n')
})
