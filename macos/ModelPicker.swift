import AppKit

struct ModelChoice { let id: String; let title: String }

@MainActor final class ModelPicker: NSButton, NSPopoverDelegate {
    private(set) var choices: [ModelChoice] = []
    private(set) var selectedID = ""
    let popover = NSPopover()
    lazy var pickerContent = ModelPickerContent(owner:self)
    override init(frame: NSRect) {
        super.init(frame:frame)
        bezelStyle = .rounded; alignment = .left; lineBreakMode = .byTruncatingMiddle
        image = NSImage(systemSymbolName:"chevron.up.chevron.down",accessibilityDescription:nil)
        imagePosition = .imageRight
        target=self; action=#selector(openPicker)
        setAccessibilityLabel("Choose model")
    }
    convenience init() { self.init(frame:.zero) }
    required init?(coder:NSCoder) { fatalError("init(coder:) unavailable") }
    func update(choices:[ModelChoice], selected:String) {
        self.choices=choices; selectedID=selected
        if !selected.isEmpty && !choices.contains(where: { $0.id == selected }) {
            self.choices.append(ModelChoice(id:selected,title:selected + " · saved, unavailable"))
        }
        title=self.choices.first(where: { $0.id == selected })?.title ?? "Choose model…"
        toolTip=title; setAccessibilityValue(title)
        if popover.isShown { pickerContent.filter() }
    }
    @objc func openPicker() {
        guard window != nil else { return }
        if popover.isShown { popover.close(); return }
        popover.contentViewController=pickerContent; popover.behavior = .transient; popover.delegate=self
        // A picker opens/closes immediately, including rapid keyboard interactions.
        popover.animates=false
        popover.appearance=effectiveAppearance
        popover.contentSize=NSSize(width:520,height:360)
        pickerContent.search.stringValue=""; pickerContent.filter()
        popover.show(relativeTo:bounds,of:self,preferredEdge:.maxY)
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        if popover.isShown { popover.appearance=effectiveAppearance }
    }
    func commit(_ choice:ModelChoice) {
        selectedID=choice.id; title=choice.title; toolTip=title; setAccessibilityValue(title)
        popover.close()
    }
    func popoverDidShow(_ notification:Notification) {
        pickerContent.view.window?.makeFirstResponder(pickerContent.search)
    }
    func popoverDidClose(_ notification:Notification) {
        if window?.isVisible == true { window?.makeFirstResponder(self) }
    }
}

private final class PickerRoot: NSView { override var isFlipped: Bool { true } }

@MainActor final class ModelPickerContent: NSViewController, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate {
    weak var owner:ModelPicker?
    let search=NSSearchField()
    let table=PickerTable()
    let scroll=NSScrollView()
    let resultLabel=NSTextField(labelWithString:"")
    let chooseButton=NSButton(title:"Choose",target:nil,action:nil)
    private(set) var visibleChoices:[ModelChoice]=[]
    init(owner:ModelPicker) {
        self.owner=owner; super.init(nibName:nil,bundle:nil)
        preferredContentSize=NSSize(width:520,height:360)
        _ = view
    }
    required init?(coder:NSCoder) { fatalError("init(coder:) unavailable") }
    override func loadView() {
        view=PickerRoot(frame:NSRect(x:0,y:0,width:520,height:360))
        search.frame=NSRect(x:16,y:14,width:488,height:28); search.placeholderString="Search models"; search.delegate=self
        search.sendsSearchStringImmediately=true; search.setAccessibilityLabel("Search models"); view.addSubview(search)
        scroll.frame=NSRect(x:16,y:54,width:488,height:248); scroll.hasVerticalScroller=true; scroll.autohidesScrollers=false; scroll.borderType = .bezelBorder
        let column=NSTableColumn(identifier:NSUserInterfaceItemIdentifier("model")); column.width=468
        table.addTableColumn(column); table.headerView=nil; table.rowHeight=28; table.allowsEmptySelection=true
        table.delegate=self; table.dataSource=self; table.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
        table.target=self; table.doubleAction=#selector(choose); table.setAccessibilityLabel("Available models")
        table.onChoose={ [weak self] in self?.choose() }; table.onCancel={ [weak self] in self?.cancelOperation(nil) }
        scroll.documentView=table; view.addSubview(scroll)
        resultLabel.frame=NSRect(x:16,y:318,width:270,height:22); resultLabel.font = .systemFont(ofSize:11); resultLabel.textColor = .secondaryLabelColor; view.addSubview(resultLabel)
        let cancel=NSButton(title:"Cancel",target:self,action:#selector(cancelOperation(_:))); cancel.frame=NSRect(x:324,y:312,width:86,height:30); view.addSubview(cancel)
        chooseButton.target=self; chooseButton.action=#selector(choose); chooseButton.bezelStyle = .rounded
        chooseButton.frame=NSRect(x:416,y:312,width:88,height:30); view.addSubview(chooseButton)
    }
    func filter() {
        let query=search.stringValue.trimmingCharacters(in:.whitespacesAndNewlines)
        visibleChoices=(owner?.choices ?? []).filter { query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) }
        table.reloadData()
        let row=visibleChoices.firstIndex(where: { $0.id == owner?.selectedID }) ?? (visibleChoices.isEmpty ? -1 : 0)
        if row >= 0 { table.selectRowIndexes(IndexSet(integer:row),byExtendingSelection:false); table.scrollRowToVisible(row) }
        else { table.deselectAll(nil) }
        chooseButton.isEnabled = row >= 0
        resultLabel.stringValue=visibleChoices.isEmpty ? (query.isEmpty ? "No models available" : "No matching models") : "\(visibleChoices.count) of \(owner?.choices.count ?? 0) models"
    }
    func controlTextDidChange(_ notification:Notification) { filter() }
    func numberOfRows(in tableView:NSTableView) -> Int { visibleChoices.count }
    func tableView(_ tableView:NSTableView,viewFor tableColumn:NSTableColumn?,row:Int) -> NSView? {
        guard visibleChoices.indices.contains(row) else { return nil }
        let text=NSTextField(labelWithString:(visibleChoices[row].id == owner?.selectedID ? "✓  " : "    ") + visibleChoices[row].title)
        text.font = .systemFont(ofSize:12); text.lineBreakMode = .byTruncatingMiddle; text.toolTip=visibleChoices[row].title
        return text
    }
    func tableViewSelectionDidChange(_ notification:Notification) { chooseButton.isEnabled=visibleChoices.indices.contains(table.selectedRow) }
    @objc func choose() {
        guard visibleChoices.indices.contains(table.selectedRow) else { return }
        owner?.commit(visibleChoices[table.selectedRow])
    }
    override func cancelOperation(_ sender:Any?) { owner?.popover.close() }
    func control(_ control:NSControl,textView:NSTextView,doCommandBy commandSelector:Selector) -> Bool {
        if commandSelector == #selector(NSResponder.insertNewline(_:)) { choose(); return true }
        if commandSelector == #selector(NSResponder.cancelOperation(_:)) { cancelOperation(nil); return true }
        if commandSelector == #selector(NSResponder.moveDown(_:)) || commandSelector == #selector(NSResponder.moveUp(_:)) {
            guard !visibleChoices.isEmpty else { return true }
            let delta=commandSelector == #selector(NSResponder.moveDown(_:)) ? 1 : -1
            let row=min(visibleChoices.count-1,max(0,table.selectedRow+delta))
            table.selectRowIndexes(IndexSet(integer:row),byExtendingSelection:false); table.scrollRowToVisible(row)
            return true
        }
        return false
    }
}

@MainActor final class PickerTable: NSTableView {
    var onChoose:(() -> Void)?
    var onCancel:(() -> Void)?
    override func keyDown(with event:NSEvent) {
        if event.keyCode == 36 || event.keyCode == 76 { onChoose?() }
        else if event.keyCode == 53 { onCancel?() }
        else { super.keyDown(with:event) }
    }
}
