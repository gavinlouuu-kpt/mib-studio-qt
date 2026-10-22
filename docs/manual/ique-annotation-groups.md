# Group planned samples by annotation

In the workspace, open **Experiment → Plan**. Fill the sample's condition,
experimental unit and other annotations. To add a custom category, use
**Key/values** in the sample editor, for example `batch` with value `A`.

The **Annotation groups** card groups the sheet by Condition, Experimental unit,
Channel, Medium or a custom key. Choose a field in **Group samples by**. The
table shows each group's sample refs, sample count, declared unit labels and
how many samples have no unit declared. Grouping never changes run order.

Repeated unit labels are not additional independent replicates. The sheet's
condition colours are reused; categories beyond its available colour slots
remain labelled without a swatch. Missing or invalid category values remain
explicit. Correct duplicate keys and other sheet errors before exporting.

**Show annotation JSON** exposes versioned JSON you can copy to export the
planned metadata and grouping. It is disabled for an invalid sheet, such as a
concentration without a unit. This export contains no measured results and
does not save or reopen an experiment. The group selection is not retained
after reloading. Editing or grouping annotations never starts acquisition or
applies hardware settings.
