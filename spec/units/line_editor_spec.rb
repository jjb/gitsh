require 'spec_helper'
require 'gitsh/line_editor'

describe Gitsh::LineEditor do
  describe '.input=' do
    it 'raises when given something other than a File' do
      expect { described_class.input = $stdin }.not_to raise_exception
      expect { described_class.input = :input }.to raise_exception(TypeError)
    end
  end
end
