// Registry mapping a property descriptor `type` -> the Svelte control component
// that renders it. Keeps PropertyForm data-driven: a new property type is one
// import + one map entry, not another if/else branch.
import type { Component } from "svelte";
import type { PropertyDescriptor } from "$lib/api/bridge";

import BoolControl from "$lib/properties/BoolControl.svelte";
import NumberControl from "$lib/properties/NumberControl.svelte";
import TextControl from "$lib/properties/TextControl.svelte";
import PathControl from "$lib/properties/PathControl.svelte";
import ListControl from "$lib/properties/ListControl.svelte";
import ColorControl from "$lib/properties/ColorControl.svelte";
import ButtonControl from "$lib/properties/ButtonControl.svelte";
import GroupControl from "$lib/properties/GroupControl.svelte";
import FontControl from "$lib/properties/FontControl.svelte";
import EditableListControl from "$lib/properties/EditableListControl.svelte";
import FrameRateControl from "$lib/properties/FrameRateControl.svelte";
import UnsupportedControl from "$lib/properties/UnsupportedControl.svelte";

// Props every control receives. `value` is the current value; `onChange` reports
// a new value for `prop.name` (the form debounces + pushes to properties.set).
// `onButton` invokes properties.button for a named button prop.
export interface ControlProps {
  prop: PropertyDescriptor;
  value: unknown;
  onChange: (name: string, value: unknown) => void;
  onButton: (name: string) => void;
  // Resolve any property's current value from the flat settings map. Groups need
  // it to render their nested children (obs_data settings are flat, not nested).
  lookup: (name: string) => unknown;
}

type Control = Component<ControlProps>;

export const controlRegistry: Record<string, Control> = {
  bool: BoolControl,
  int: NumberControl,
  float: NumberControl,
  text: TextControl,
  path: PathControl,
  list: ListControl,
  color: ColorControl,
  color_alpha: ColorControl,
  button: ButtonControl,
  group: GroupControl,
  font: FontControl,
  editable_list: EditableListControl,
  frame_rate: FrameRateControl,
};

export function controlFor(type: string): Control {
  return controlRegistry[type] ?? UnsupportedControl;
}
