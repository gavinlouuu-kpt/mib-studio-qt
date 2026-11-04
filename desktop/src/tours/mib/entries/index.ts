// MIB Studio tours: the six fold-in tours, in the order the Tours list shows them.
import type { TourEntry } from "../../registry";
import { e01Discovery } from "./e01Discovery";
import { e02Features } from "./e02Features";
import { e04e06Runs } from "./e04e06Runs";
import { i02Recipes } from "./i02Recipes";
import { f04Population } from "./f04Population";
import { i12f12Workspace } from "./i12f12Workspace";

export const MIB_TOURS: TourEntry[] = [e01Discovery, e02Features, e04e06Runs, i02Recipes, f04Population, i12f12Workspace];
export { e01Discovery, e02Features, e04e06Runs, i02Recipes, f04Population, i12f12Workspace };
