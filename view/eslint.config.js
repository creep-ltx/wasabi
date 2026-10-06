// @ts-check
import js from '@eslint/js';
import tseslint from 'typescript-eslint';
import reactHooks from 'eslint-plugin-react-hooks';
import globals from 'globals';

// Design-system rules. Raw colours and sizes live only in src/tokens.css;
// the page is built from Radix components only, never raw HTML elements.
const rawValue =
  '/(#[0-9a-fA-F]{3,8}\\b)|(\\b\\d+(\\.\\d+)?(px|pt|rem|em|vh|vw)\\b)|(\\b(rgb|rgba|hsl|hsla|hwb|oklch|oklab)\\()/';

export default tseslint.config(
  { ignores: ['dist', 'node_modules'] },
  {
    files: ['**/*.{ts,tsx,js}'],
    extends: [js.configs.recommended, ...tseslint.configs.recommended],
    languageOptions: {
      ecmaVersion: 2022,
      globals: { ...globals.browser, ...globals.node },
    },
  },
  {
    files: ['src/**/*.{ts,tsx}'],
    extends: [reactHooks.configs.flat.recommended],
    rules: {
      'no-restricted-syntax': [
        'error',
        {
          selector: `Literal[value=${rawValue}]`,
          message: 'Raw colours and sizes belong in src/tokens.css; use a token.',
        },
        {
          selector: `TemplateElement[value.raw=${rawValue}]`,
          message: 'Raw colours and sizes belong in src/tokens.css; use a token.',
        },
        {
          selector: 'TemplateElement[value.raw=/^(px|pt|rem|em|vh|vw)\\b/]',
          message: 'Raw colours and sizes belong in src/tokens.css; use a token.',
        },
        {
          // Radix has no chart or image primitives: the sparkline's SVG
          // parts and screenshot <img> are the only raw elements allowed.
          selector: 'JSXOpeningElement[name.name=/^(?!(svg|path|line|circle|rect|g|title|img)$)[a-z]/]',
          message: 'Use a Radix component, not a raw HTML element.',
        },
        {
          selector: 'JSXAttribute[name.name="style"] Property:not([key.value=/^--wv-/])',
          message: 'Inline styles may only set --wv-* custom properties; put the rest in CSS with tokens.',
        },
      ],
    },
  },
);
