require 'spec_helper'
require 'gitsh/line_editor_blank_filter'

describe LineEditorBlankFilter do
  describe '#readline' do
    let(:fake_line_editor) { FakeLineEditor.new }
    before { fake_line_editor::HISTORY.clear }

    it 'enters non-blank lines into the history' do
      line_editor_blank_filter = described_class.new(fake_line_editor)

      fake_line_editor.type('hello')
      line_editor_blank_filter.readline('>', true)

      expect(line_editor_blank_filter::HISTORY.to_a).to eq(['hello'])
    end

    it 'does not enter blank lines into the history' do
      line_editor_blank_filter = described_class.new(fake_line_editor)

      fake_line_editor.type('')
      line_editor_blank_filter.readline('>', true)

      expect(line_editor_blank_filter::HISTORY.to_a).to be_empty
    end

    it 'does not enter lines consisting of only whitespace into the history' do
      line_editor_blank_filter = described_class.new(fake_line_editor)

      fake_line_editor.type("\n   \n")
      line_editor_blank_filter.readline('>', true)

      expect(line_editor_blank_filter::HISTORY.to_a).to be_empty
    end

    it 'does not enter nil lines into the history' do
      line_editor_blank_filter = described_class.new(fake_line_editor)

      fake_line_editor.send_eof
      line_editor_blank_filter.readline('>', true)

      expect(line_editor_blank_filter::HISTORY.to_a).to be_empty
    end

    it 'does not #pop the last entry to history if add_hist is false' do
      line_editor_blank_filter = described_class.new(fake_line_editor)

      fake_line_editor.type('hello')
      line_editor_blank_filter.readline('>', true)
      fake_line_editor.type(' ')
      line_editor_blank_filter.readline('>', false)

      expect(line_editor_blank_filter::HISTORY.to_a).to eq(['hello'])
    end
  end
end
