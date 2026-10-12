import { execFileSync } from 'node:child_process'
import { readFile, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { clangFormatPath } from 'clang-format-node'
import lintStaged from 'lint-staged'
import * as prettier from 'prettier'

const scriptPath = fileURLToPath(import.meta.url)
const repositoryRoot = path.resolve(path.dirname(scriptPath), '../../..')
const prettierIgnore = path.join(repositoryRoot, '.prettierignore')
const cppPattern = /\.(?:c|cpp|h|hpp)$/i
const prettierPattern = /\.(?:[cm]?js|jsx|ts|tsx|vue|css|scss|json|jsonc|md|ya?ml|html)$/i
const excludedPattern = /(?:^|\/)(?:node_modules|\.pio|data)(?:\/|$)|^protocol\/generated\//

async function formatFiles(files, { check = false } = {}) {
  let clean = true

  for (const file of files) {
    const absolutePath = path.resolve(repositoryRoot, file)
    const relativePath = path.relative(repositoryRoot, absolutePath).replaceAll('\\', '/')

    if (relativePath.startsWith('../') || excludedPattern.test(relativePath)) continue

    const isCpp = cppPattern.test(relativePath)

    if (!isCpp && !prettierPattern.test(relativePath)) continue

    if (!isCpp) {
      const info = await prettier.getFileInfo(absolutePath, { ignorePath: prettierIgnore })

      if (info.ignored) continue
    }

    const original = await readFile(absolutePath, 'utf8')
    const formatted = isCpp
      ? execFileSync(clangFormatPath, [`--assume-filename=${absolutePath}`], {
          input: original,
          encoding: 'utf8',
          maxBuffer: 16 * 1024 * 1024,
        })
      : await prettier.format(original, {
          ...(await prettier.resolveConfig(absolutePath, { editorconfig: true })),
          filepath: absolutePath,
        })

    if (original === formatted) continue

    clean = false
    console.log(`${check ? 'Needs formatting' : 'Formatted'}: ${relativePath}`)

    if (!check) await writeFile(absolutePath, formatted)
  }

  return clean
}

async function main() {
  const [mode, ...files] = process.argv.slice(2)

  if (mode === '--staged') {
    const success = await lintStaged({
      cwd: repositoryRoot,
      concurrent: false,
      config: {
        '*.{c,cpp,h,hpp,js,cjs,mjs,jsx,ts,tsx,vue,css,scss,json,jsonc,md,yml,yaml,html}': {
          title: 'Format staged files',
          task: async (stagedFiles) => {
            await formatFiles(stagedFiles)
          },
        },
      },
    })

    if (!success) process.exitCode = 1

    return
  }

  if (mode !== '--write' && mode !== '--check')
    throw new Error(
      'Usage: node gateway/ui/scripts/format.mjs --write|--check [files...] | --staged',
    )

  const targets = files.length
    ? files
    : execFileSync('git', ['ls-files', '-z'], { cwd: repositoryRoot, encoding: 'utf8' })
        .split('\0')
        .filter(Boolean)
  const clean = await formatFiles(targets, { check: mode === '--check' })

  if (mode === '--check' && !clean) process.exitCode = 1
}

if (process.argv[1] && path.resolve(process.argv[1]) === scriptPath) await main()
