const globals = require("globals");
const js = require("@eslint/js");

const {
    FlatCompat,
} = require("@eslint/eslintrc");

const compat = new FlatCompat({
    baseDirectory: __dirname,
    recommendedConfig: js.configs.recommended,
    allConfig: js.configs.all
});

module.exports = [...compat.extends("eslint:recommended"), {
    languageOptions: {
        globals: {
            ...globals.node,
        },
    },

    rules: {
        indent: ["error", 4, {
            SwitchCase: 1,
        }],

        "linebreak-style": ["error", "unix"],
        "no-console": "off",
        quotes: ["error", "single"],
        semi: ["error", "always"],
    },
}];