# Anti-slop rules (Basalt)

## Visual

- No default purple/blue gradients. Solid dark surfaces, subtle tone shifts only.
- No glow on cards or buttons. One accent element may glow.
- No heavy drop shadows on flush tiles. Use 1px borders or a faint elevation.
- Vary radii: tiles 12-16, inputs 6, pills 9999. Don't use one value everywhere.
- Red/green only for P&L and price direction. Accent color is separate.
- Chart colors, fonts and grid lines must match the palette, not library defaults.

## Layout (bento)

- No equal-size tile grids. Mix 2x2, 2x1, 1x2, 1x1. One hero tile.
- Vary tile padding by importance. No uniform spacing everywhere.

## Type

- Don't use the same font for display and body. Pair them.
- Tabular numerals on every number that updates.
- Sentence case for labels and buttons. All-caps only for tiny labels.
- Headlines under 4 lines.

## Content

- No round placeholder numbers (1,000 / 10,000 / $99.99). Use realistic ones: $84,213.47, +2.31%.
- No "John Doe", no lorem ipsum, no emoji as icons or headings.
- No filler verbs (empower, unlock, leverage). Use Buy, Sell, Track, Watch.

## Interaction

- Every control has hover, focus, active and disabled states.
- Animate transform and opacity only, never width/height.
- Routine actions 100-150ms, bigger transitions 200-300ms. No linear easing.
- No fade-up-on-scroll on everything. No parallax.
- No continuous animation (pulsing dots, spinners) unless it shows live state.
- Skeleton loaders shaped like the real content, not a generic spinner.
- Hover can't be the only way to reach anything on touch.

## Components

- One icon library, one stroke weight (Lucide or Phosphor).
- One primary button style, one secondary, one tertiary.
- One input style everywhere.

## Pre-flight (fix before done)

- [ ] No gradient, glow or emoji slop
- [ ] Bento tiles vary in size
- [ ] Numbers are tabular and realistic
- [ ] All states and focus rings exist
- [ ] Motion is calm and transform/opacity only
